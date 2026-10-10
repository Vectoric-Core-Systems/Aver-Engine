#include "aver/game/LevelStreaming.hpp"

#if AVER_MODULE_SCENE
#  include "aver/game/GameContent.hpp"
#  include "aver/core/Hash.hpp"
#  include "aver/core/Log.hpp"
#  include "aver/scene/scene_abi.h"
#  include "aver/world/LevelTransform.hpp"
#  if AVER_MODULE_PHYSICS
#    include "aver/physics/physics_abi.h"
#  endif

#  include <algorithm>
#  include <chrono>
#  include <cfloat>

namespace aver::game {

namespace {
// A placement with no known bounds (no aabb= and its mesh not loaded) never leaves: residency cannot
// be judged without them, and unloading what might be under the viewer is the worse failure.
constexpr f32 kUnknownExtentCm = 1.0e9f;

Transform localOf(const fmt::OcWorldPlacement& p) {
    Transform xf;
    xf.position = Vec3{static_cast<f32>(p.x), static_cast<f32>(p.y), static_cast<f32>(p.z)};
    xf.rotation = world::quatFromEulerDeg(Vec3{static_cast<f32>(p.roll), static_cast<f32>(p.pitch),
                                               static_cast<f32>(p.yaw)});
    xf.scale = Vec3{static_cast<f32>(p.sx), static_cast<f32>(p.sy), static_cast<f32>(p.sz)};
    return xf;
}
} // namespace

void LevelStreaming::begin(const fmt::OcWorldData& w, GameContent& content, rhi::IDevice& device,
                           const world::InstantiateOptions& opt, Hooks hooks) {
    content_ = &content;
    device_ = &device;
    opt_ = opt;
    opt_.progress = nullptr;
    hooks_ = std::move(hooks);
    records_ = w.placements;
    removed_.assign(records_.size(), 0);
    itemOf_.assign(records_.size(), ~0u);
    items_.clear();
    rootPlacements_.clear();
    placementOf_.clear();
    entityOf_.clear();
    animatedByItem_.clear();
    animated_.clear();
    materialsBound_.clear();

    // Roots own their descendants (parents precede children in OcWorldData). A child of a class
    // placement loads as a root, as world::instantiate already treats it.
    for (u32 i = 0; i < records_.size(); ++i) {
        const fmt::OcWorldPlacement& p = records_[i];
        if (!p.className.empty()) continue;
        const bool child = p.parent >= 0 && static_cast<u32>(p.parent) < i && itemOf_[static_cast<u32>(p.parent)] != ~0u;
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
    std::vector<world::Aabb> bounds;
    bounds.reserve(items_.size());
    usize unknown = 0;
    for (const Item& it : items_) {
        bounds.push_back(itemBounds(it));
        if (bounds.back().max.x - bounds.back().min.x >= kUnknownExtentCm) ++unknown;
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
    releaseUnusedShapes();
#  if AVER_MODULE_PHYSICS
    for (const auto& [mesh, shape] : meshShapes_) if (shape) aver_phys_release_mesh_shape(shape);
#  endif
    meshShapes_.clear();
    active_ = false;
    records_.clear();
    items_.clear();
    placementOf_.clear();
    entityOf_.clear();
    animated_.clear();
}

bool LevelStreaming::worldBounds(u32 placement, Vec3& outMin, Vec3& outMax) const {
    const fmt::OcWorldPlacement& p = records_[placement];
    if (p.hasBounds) {
        outMin = Vec3{p.boundsMin[0], p.boundsMin[1], p.boundsMin[2]};
        outMax = Vec3{p.boundsMax[0], p.boundsMax[1], p.boundsMax[2]};
        return true;
    }
    const std::pair<Vec3, Vec3>* lb = content_ ? content_->boundsFor(p.objectId) : nullptr;
    if (!lb) return false;
    // World transform through the parent chain, composed as world::instantiate composes it.
    Transform xf = localOf(p);
    for (i32 q = p.parent; q >= 0 && static_cast<usize>(q) < records_.size(); q = records_[static_cast<usize>(q)].parent) {
        const Transform pw = localOf(records_[static_cast<usize>(q)]);
        Transform out;
        out.scale = Vec3{pw.scale.x * xf.scale.x, pw.scale.y * xf.scale.y, pw.scale.z * xf.scale.z};
        out.rotation = pw.rotation * xf.rotation;
        const Vec3 scaled{xf.position.x * pw.scale.x, xf.position.y * pw.scale.y, xf.position.z * pw.scale.z};
        out.position = pw.position + pw.rotation.rotate(scaled);
        xf = out;
    }
    outMin = Vec3{FLT_MAX, FLT_MAX, FLT_MAX};
    outMax = Vec3{-FLT_MAX, -FLT_MAX, -FLT_MAX};
    for (int c = 0; c < 8; ++c) {
        const Vec3 l{(c & 1) ? lb->second.x : lb->first.x, (c & 2) ? lb->second.y : lb->first.y,
                     (c & 4) ? lb->second.z : lb->first.z};
        const Vec3 s{l.x * xf.scale.x, l.y * xf.scale.y, l.z * xf.scale.z};
        const Vec3 v = xf.position + xf.rotation.rotate(s);
        outMin = Vec3{std::min(outMin.x, v.x), std::min(outMin.y, v.y), std::min(outMin.z, v.z)};
        outMax = Vec3{std::max(outMax.x, v.x), std::max(outMax.y, v.y), std::max(outMax.z, v.z)};
    }
    return true;
}

world::Aabb LevelStreaming::itemBounds(const Item& it) const {
    world::Aabb b;
    b.min = Vec3{FLT_MAX, FLT_MAX, FLT_MAX};
    b.max = Vec3{-FLT_MAX, -FLT_MAX, -FLT_MAX};
    bool any = false;
    for (const u32 pi : it.placements) {
        if (removed(pi)) continue;
        Vec3 lo, hi;
        if (!worldBounds(pi, lo, hi)) {
            b.min = Vec3{-kUnknownExtentCm, -kUnknownExtentCm, -kUnknownExtentCm};
            b.max = Vec3{kUnknownExtentCm, kUnknownExtentCm, kUnknownExtentCm};
            return b;
        }
        b.min = Vec3{std::min(b.min.x, lo.x), std::min(b.min.y, lo.y), std::min(b.min.z, lo.z)};
        b.max = Vec3{std::max(b.max.x, hi.x), std::max(b.max.y, hi.y), std::max(b.max.z, hi.z)};
        any = true;
    }
    if (!any) b.min = b.max = Vec3{0, 0, 0};
    return b;
}

void LevelStreaming::bindMeshMaterials(u64 meshId) {
#if AVER_MODULE_PBR
    if (!materialsBound_.emplace(meshId, true).second) return;
    // A mesh's own slot materials, as GameLevel::load binds them for an eager level.
    const i32 slot0Token = content_->meshDefaultMaterial(meshId);
    if (slot0Token) {
        const std::string& name = content_->meshSlot0Name(meshId);
        if (!name.empty())
            if (const pbr::MaterialHandle h = content_->materialForSurface(name)) content_->bindSurfaceMaterial(slot0Token, h);
    }
    if (const std::vector<GameContent::MeshPart>* parts = content_->partsFor(meshId)) {
        for (const GameContent::MeshPart& part : *parts) {
            if (!part.material) continue;
            const char* partName = aver_scene_material_name(part.material);
            if (!partName || !*partName) continue;
            if (const pbr::MaterialHandle h = content_->materialForSurface(partName))
                content_->bindSurfaceMaterial(part.material, h);
        }
    }
#else
    (void)meshId;
#endif
}

void LevelStreaming::loadItems(scene::World& world, const std::vector<u32>& items) {
    (void)world;   // instantiate() creates into the process-global World
    // One instantiate for the batch: a sub-level of the items' placements, parents remapped.
    fmt::OcWorldData sub;
    std::vector<u32> globalOf;
    std::unordered_map<u32, i32> localOf;
    for (const u32 item : items) {
        Item& it = items_[item];
        for (const u32 pi : it.placements) {
            if (removed(pi)) continue;
            fmt::OcWorldPlacement p = records_[pi];
            const auto par = p.parent >= 0 ? localOf.find(static_cast<u32>(p.parent)) : localOf.end();
            p.parent = par != localOf.end() ? par->second : -1;
            localOf[pi] = static_cast<i32>(sub.placements.size());
            sub.placements.push_back(std::move(p));
            globalOf.push_back(pi);
            const u64 mesh = records_[pi].objectId;
            if (std::find(it.meshes.begin(), it.meshes.end(), mesh) == it.meshes.end()) {
                if (content_->acquireMesh(*device_, mesh)) {
                    it.meshes.push_back(mesh);
                    bindMeshMaterials(mesh);
                    // Built on the prefetch worker; instantiate reuses it instead of building one here.
                    if (!meshShapes_.count(mesh))
                        if (const i32 shape = content_->takeMeshShape(mesh)) meshShapes_.emplace(mesh, shape);
                }
            }
        }
    }
    opt_.meshShapes = &meshShapes_;
    const world::LevelInstance inst = world::instantiate(sub, opt_);
    std::vector<scene::Entity> loaded;
    std::vector<u32> loadedPlacement;
    std::vector<i32> loadedBody;
    for (usize k = 0; k < inst.entities.size(); ++k) {
        const u32 pi = globalOf[inst.placementIndex[k]];
        Item& it = items_[itemOf_[pi]];
        it.entities.push_back(inst.entities[k]);
        const i32 body = k < inst.entityBody.size() ? inst.entityBody[k] : -1;
        if (body >= 0) it.bodies.emplace_back(inst.entities[k], body);
        loadedBody.push_back(body);
        placementOf_[inst.entities[k]] = pi;
        entityOf_[pi] = inst.entities[k];
        loaded.push_back(inst.entities[k]);
        loadedPlacement.push_back(pi);
    }
    for (const world::AnimatedBody& ab : inst.animatedBodies) {
        const auto pl = placementOf_.find(ab.entity);
        if (pl != placementOf_.end()) animatedByItem_[itemOf_[pl->second]].push_back(ab);
    }
    for (const u32 item : items) {
        items_[item].loaded = true;
        streamer_.markLoaded(item);
    }
    if (!inst.animatedBodies.empty()) rebuildAnimated();
    if (hooks_.afterLoad && !loaded.empty()) hooks_.afterLoad(loaded, loadedPlacement, loadedBody);
}

void LevelStreaming::evictItem(scene::World& world, u32 item, bool writeBack) {
    Item& it = items_[item];
    for (const scene::Entity e : it.entities) {
        const auto pl = placementOf_.find(e);
        if (pl != placementOf_.end() && hooks_.beforeEvict && world.valid(e)) hooks_.beforeEvict(e, pl->second, writeBack);
    }
#if AVER_MODULE_PHYSICS
    for (const auto& eb : it.bodies) if (eb.second >= 0) aver_phys_remove_body(eb.second);
#endif
    // Children first, so no destroy sees a parent already gone.
    for (auto e = it.entities.rbegin(); e != it.entities.rend(); ++e) {
        const auto pl = placementOf_.find(*e);
        if (pl != placementOf_.end()) { entityOf_.erase(pl->second); placementOf_.erase(pl); }
        if (world.valid(*e)) world.destroy(*e);
    }
    for (const u64 mesh : it.meshes) content_->releaseMesh(*device_, mesh);
    it.entities.clear();
    it.bodies.clear();
    it.meshes.clear();
    it.loaded = false;
    if (animatedByItem_.erase(item)) rebuildAnimated();
    streamer_.markEvicted(item);
}

void LevelStreaming::rebuildAnimated() {
    animated_.clear();
    for (const auto& [item, list] : animatedByItem_) animated_.insert(animated_.end(), list.begin(), list.end());
}

void LevelStreaming::releaseUnusedShapes() {
#  if AVER_MODULE_PHYSICS
    for (auto it = meshShapes_.begin(); it != meshShapes_.end();) {
        if (content_->meshLoaded(it->first)) { ++it; continue; }
        if (it->second) aver_phys_release_mesh_shape(it->second);   // live bodies keep their own reference
        it = meshShapes_.erase(it);
    }
#  endif
}

void LevelStreaming::tick(scene::World& world, const std::vector<Vec3>& viewers) {
    if (!active_) return;
    std::vector<u32> toLoad, toEvict;
    streamer_.update(viewers, toLoad, toEvict);
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
    if (lastEvicted_) releaseUnusedShapes();

    // Pinned roots load now; the rest once their meshes are read.
    std::vector<u32> now, ready;
    f32 nearestMissing = FLT_MAX;
    for (const u32 item : toLoad) {
        if (item >= items_.size() || items_[item].loaded) continue;
        if (streamer_.pinned(item)) { now.push_back(item); continue; }
        bool read = true;
        for (const u32 pi : items_[item].placements) {
            if (removed(pi)) continue;
            content_->prefetchMesh(records_[pi].objectId);
            read = read && content_->meshReady(records_[pi].objectId);
        }
        nearestMissing = std::min(nearestMissing, streamer_.distance(item, viewers));
        if (read) ready.push_back(item);
    }
    if (!now.empty()) {
        loadItems(world, now);
        lastLoaded_ += static_cast<u32>(now.size());
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
        loadItems(world, one);
        ++lastLoaded_;
    }
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
    if (placement < itemOf_.size() && itemOf_[placement] != ~0u) streamer_.pin(itemOf_[placement], on);
}

void LevelStreaming::unpinAll() {
    for (u32 i = 0; i < items_.size(); ++i) streamer_.pin(i, false);
}

void LevelStreaming::updateRecord(u32 placement, fmt::OcWorldPlacement p, bool keepParent) {
    if (placement >= records_.size()) return;
    if (keepParent) p.parent = records_[placement].parent;
    records_[placement] = p;
    // Fresh bounds from the loaded mesh when there is one, so the saved .ocstream follows the edit.
    records_[placement].hasBounds = false;
    Vec3 lo, hi;
    if (worldBounds(placement, lo, hi)) {
        fmt::OcWorldPlacement& r = records_[placement];
        r.hasBounds = true;
        r.boundsMin[0] = lo.x; r.boundsMin[1] = lo.y; r.boundsMin[2] = lo.z;
        r.boundsMax[0] = hi.x; r.boundsMax[1] = hi.y; r.boundsMax[2] = hi.z;
    } else if (p.hasBounds) {
        records_[placement].hasBounds = true;   // mesh not loaded: keep the bounds it had
    }
    if (itemOf_[placement] != ~0u) streamer_.setBounds(itemOf_[placement], itemBounds(items_[itemOf_[placement]]));
}

u32 LevelStreaming::addRecord(const fmt::OcWorldPlacement& p, scene::Entity e) {
    const u32 pi = static_cast<u32>(records_.size());
    records_.push_back(p);
    records_.back().parent = -1;
    removed_.push_back(0);
    Item item;
    item.placements.push_back(pi);
    item.entities.push_back(e);
    item.loaded = true;
    if (content_->acquireMesh(*device_, p.objectId)) item.meshes.push_back(p.objectId);
    const u32 idx = static_cast<u32>(items_.size());
    itemOf_.push_back(idx);
    rootPlacements_.push_back(pi);
    items_.push_back(std::move(item));
    placementOf_[e] = pi;
    entityOf_[pi] = e;
    updateRecord(pi, records_[pi]);
    const u32 sidx = streamer_.add(itemBounds(items_[idx]));
    (void)sidx;   // the streamer numbers items in add order, as items_ does
    streamer_.markLoaded(idx);
    streamer_.pin(idx, true);
    return pi;
}

void LevelStreaming::removeRecord(u32 placement) {
    if (placement >= records_.size() || removed(placement)) return;
    const u32 item = itemOf_[placement];
    if (item == ~0u) { removed_[placement] = 1; return; }
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
            it.entities.erase(std::remove(it.entities.begin(), it.entities.end(), gone), it.entities.end());
            it.bodies.erase(std::remove_if(it.bodies.begin(), it.bodies.end(),
                                           [gone](const auto& eb) { return eb.first == gone; }), it.bodies.end());
            if (auto an = animatedByItem_.find(item); an != animatedByItem_.end()) {
                an->second.erase(std::remove_if(an->second.begin(), an->second.end(),
                                                [gone](const world::AnimatedBody& ab) { return ab.entity == gone; }),
                                 an->second.end());
            }
            entityOf_.erase(e);
        }
    }
    rebuildAnimated();
    if (isRoot) {
        for (const u64 mesh : it.meshes) content_->releaseMesh(*device_, mesh);
        it.meshes.clear();
        it.loaded = false;
        streamer_.remove(item);
    } else {
        streamer_.setBounds(item, itemBounds(it));
    }
}

} // namespace aver::game

#endif // AVER_MODULE_SCENE
