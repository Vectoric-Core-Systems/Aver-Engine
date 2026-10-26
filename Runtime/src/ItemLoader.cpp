#include "aver/game/ItemLoader.hpp"
#include "aver/core/HitchMarks.hpp"

#if AVER_MODULE_SCENE
#  include "aver/game/GameContent.hpp"
#  include "aver/game/LevelComponents.hpp"
#  if AVER_MODULE_PHYSICS
#    include "aver/physics/physics_abi.h"
#  endif

#  include <algorithm>

namespace aver::game {

void ItemLoader::begin(GameContent& content, rhi::IDevice& device, const world::InstantiateOptions& opt) {
    content_ = &content;
    device_ = &device;
    opt_ = opt;
    opt_.progress = nullptr;
    items_.clear();
    animated_.clear();
}

void ItemLoader::end() {
#  if AVER_MODULE_PHYSICS
    for (const auto& [mesh, shape] : meshShapes_) if (shape) aver_phys_release_mesh_shape(shape);
#  endif
    meshShapes_.clear();
    items_.clear();
    animated_.clear();
}

void ItemLoader::bindMeshMaterials(u64 meshId) {
#if AVER_MODULE_PBR
    // A mesh's own slot materials, once per load. Queued and drained by load(), so each is built as it is made.
    content_->queueMeshMaterials(meshId);
#else
    (void)meshId;
#endif
}

std::vector<ItemLoader::Loaded> ItemLoader::load(const std::vector<Entry>& entries, world::OriginCm origin,
                                                 HitchMarks* hm) {
    // One instantiate for the batch: a sub-level of the entries' placements, parents already remapped by the caller.
    fmt::OcWorldData sub;
    sub.placements.reserve(entries.size());
    std::vector<u64> toBind;
    for (const Entry& en : entries) {
        fmt::OcWorldPlacement p = *en.rec;
        p.parent = en.parent;
        sub.placements.push_back(std::move(p));
        const u64 mesh = en.rec->objectId;
        ItemState& st = items_[en.item];
        if (std::find(st.meshes.begin(), st.meshes.end(), mesh) == st.meshes.end()) {
            content_->acquireTag = "level";
            const bool got = content_->acquireMesh(*device_, mesh);
            content_->acquireTag = nullptr;
            if (got) {
                st.meshes.push_back(mesh);
                toBind.push_back(mesh);
                // Built on the prefetch worker; instantiate reuses it instead of building one here.
                if (!meshShapes_.count(mesh))
                    if (const i32 shape = content_->takeMeshShape(mesh)) meshShapes_.emplace(mesh, shape);
            }
        }
    }
    if (hm) hm->mark("meshes");
    for (const u64 mesh : toBind) bindMeshMaterials(mesh);
#if AVER_MODULE_PBR
    while (content_->buildNextMaterial()) {}   // inside the caller's budget, before the entities that show them
#endif
    if (hm) hm->mark("materials");
    opt_.meshShapes = &meshShapes_;
    opt_.origin = origin;
    const world::LevelInstance inst = world::instantiate(sub, opt_);
    if (hm) hm->mark("instantiate");

    std::vector<Loaded> loaded;
    loaded.reserve(inst.entities.size());
    std::unordered_map<u32, u32> itemOfEntity;
    for (usize k = 0; k < inst.entities.size(); ++k) {
        const Entry& en = entries[inst.placementIndex[k]];
        ItemState& st = items_[en.item];
        st.entities.push_back(inst.entities[k]);
        const i32 body = k < inst.entityBody.size() ? inst.entityBody[k] : -1;
        if (body >= 0) st.bodies.emplace_back(inst.entities[k], body);
        itemOfEntity[inst.entities[k]] = en.item;
        loaded.push_back(Loaded{inst.entities[k], en.tag, en.item, body, en.rec});
    }
    for (const world::AnimatedBody& ab : inst.animatedBodies) {
        const auto it = itemOfEntity.find(ab.entity);
        if (it != itemOfEntity.end()) items_[it->second].animated.push_back(ab);
    }
    if (!inst.animatedBodies.empty()) rebuildAnimated();
    return loaded;
}

void ItemLoader::applyComponents(const std::vector<Loaded>& loaded) {
    // Warnings only: an INFO line per streamed item would be noise here.
    for (const Loaded& l : loaded)
        if (l.rec && !l.rec->components.empty()) applyPlacedComponents(scene::World::instance(), l.entity, *l.rec, l.tag);
}

void ItemLoader::evict(scene::World& world, u32 item, HitchMarks* hm) {
    const auto found = items_.find(item);
    if (found == items_.end()) return;
    ItemState& st = found->second;
#if AVER_MODULE_PHYSICS
    for (const auto& eb : st.bodies) if (eb.second >= 0) aver_phys_remove_body(eb.second);
#endif
    // Children first, so no destroy sees a parent already gone.
    for (auto e = st.entities.rbegin(); e != st.entities.rend(); ++e)
        if (world.valid(*e)) world.destroy(*e);
    if (hm) hm->mark("evict:destroy");
    for (const u64 mesh : st.meshes) content_->releaseMesh(*device_, mesh);
    if (hm) hm->mark("evict:releaseMesh");
    const bool hadAnimated = !st.animated.empty();
    items_.erase(found);
    if (hadAnimated) rebuildAnimated();
}

void ItemLoader::adopt(u32 item, scene::Entity e, u64 meshObjectId) {
    ItemState& st = items_[item];
    st.entities.push_back(e);
    if (content_->acquireMesh(*device_, meshObjectId)) st.meshes.push_back(meshObjectId);
}

void ItemLoader::forget(u32 item, scene::Entity e) {
    const auto found = items_.find(item);
    if (found == items_.end()) return;
    ItemState& st = found->second;
    st.entities.erase(std::remove(st.entities.begin(), st.entities.end(), e), st.entities.end());
    st.bodies.erase(std::remove_if(st.bodies.begin(), st.bodies.end(),
                                   [e](const auto& eb) { return eb.first == e; }), st.bodies.end());
    st.animated.erase(std::remove_if(st.animated.begin(), st.animated.end(),
                                     [e](const world::AnimatedBody& ab) { return ab.entity == e; }), st.animated.end());
    rebuildAnimated();
}

void ItemLoader::releaseMeshes(u32 item) {
    const auto found = items_.find(item);
    if (found == items_.end()) return;
    for (const u64 mesh : found->second.meshes) content_->releaseMesh(*device_, mesh);
    found->second.meshes.clear();
}

const std::vector<scene::Entity>& ItemLoader::entitiesOf(u32 item) const {
    static const std::vector<scene::Entity> kNone;
    const auto found = items_.find(item);
    return found == items_.end() ? kNone : found->second.entities;
}

void ItemLoader::rebuildAnimated() {
    animated_.clear();
    for (const auto& [item, st] : items_) animated_.insert(animated_.end(), st.animated.begin(), st.animated.end());
}

void ItemLoader::releaseUnusedShapes() {
#  if AVER_MODULE_PHYSICS
    for (auto it = meshShapes_.begin(); it != meshShapes_.end();) {
        if (content_->meshLoaded(it->first)) { ++it; continue; }
        if (it->second) aver_phys_release_mesh_shape(it->second);   // live bodies keep their own reference
        it = meshShapes_.erase(it);
    }
#  endif
}

void ItemLoader::prefetchTick() { content_->prefetchTick(); }
void ItemLoader::prefetch(u64 meshObjectId, f32 priority) { content_->prefetchMesh(meshObjectId, priority); }
bool ItemLoader::ready(u64 meshObjectId) const { return content_->meshReady(meshObjectId); }

} // namespace aver::game

#endif // AVER_MODULE_SCENE
