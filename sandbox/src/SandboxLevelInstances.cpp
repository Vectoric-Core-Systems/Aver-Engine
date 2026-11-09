// The editor's side of level instances (docs/LEVEL_INSTANCES.md): the hooks that keep levelEntities_, labels and bodies in
// step with what game::LevelInstances creates and destroys, the capture a commit and Create-from-Selection use, Open Level
// and Back, Bake Pack, and the stable placement ids a save keeps. The behaviour itself is LevelInstanceEditorModel's.
#include "SandboxApp.hpp"

#if AVER_MODULE_SCENE

#include "aver/formats/OcStream.hpp"
#include "aver/scene/scene_abi.h"
#include "aver/world/LevelTransform.hpp"
#if AVER_MODULE_PHYSICS
#  include "aver/physics/physics_abi.h"
#endif

#include <algorithm>
#include <filesystem>

namespace aver {

void SandboxApp::installLevelInstanceHooks() {
    game::LevelInstances::Hooks h;
    // A new entity (the instance root, a member, a light, a prefab root): the level's entity list, an outliner label, the
    // static body instantiate made for it.
    h.created = [this](scene::Entity e, i32 body, const fmt::OcWorldPlacement* p) {
        scene::World& w = scene::World::instance();
        if (std::find(lvl_.levelEntities_.begin(), lvl_.levelEntities_.end(), e) == lvl_.levelEntities_.end()) lvl_.levelEntities_.push_back(e);
        if (p) {
            // The label without the ordinal makeEntityLabel would count: instance members must not shift the level's own.
            const std::string base = entityLabelBase(p->material, p->asset);
            lvl_.entityLabels_[static_cast<u32>(e)] = p->name.empty() ? base : p->name;
            lvl_.entityAuthoredName_[static_cast<u32>(e)] = p->name;
            lvl_.entityCollide_[static_cast<u32>(e)] = p->collide;
            if (!p->animClip.empty()) lvl_.entityAnim_[static_cast<u32>(e)] = EntityAnim{p->animClip, p->animSpeed, p->animTime, p->animOnce};
        } else {
            lvl_.entityLabels_[static_cast<u32>(e)] = w.name(e);
        }
#if AVER_MODULE_PHYSICS
        if (body > 0) {
            lvl_.entityBodies_[static_cast<u32>(e)] = body;
            lvl_.levelBodies_.push_back(body);
            ++dbgOv_.colliderRev_;
        }
#else
        (void)body;
#endif
    };
    h.destroying = [this](scene::Entity e) {
        lvl_.levelEntities_.erase(std::remove(lvl_.levelEntities_.begin(), lvl_.levelEntities_.end(), e), lvl_.levelEntities_.end());
        lvl_.entityLabels_.erase(static_cast<u32>(e));
        lvl_.entityCollide_.erase(static_cast<u32>(e));
        lvl_.entityAnim_.erase(static_cast<u32>(e));
        lvl_.entitySnapZ_.erase(static_cast<u32>(e));
        lvl_.entityPlaceId_.erase(static_cast<u32>(e));
        lvl_.entityAuthoredName_.erase(static_cast<u32>(e));
        if (selectionUi_.sel_ == SelectionState::kSelScene && e == selectionUi_.selEntity_) { selectionUi_.sel_ = -1; selectionUi_.selEntity_ = scene::kInvalidEntity; }
#if AVER_MODULE_PHYSICS
        if (const auto it = lvl_.entityBodies_.find(static_cast<u32>(e)); it != lvl_.entityBodies_.end()) {
            aver_phys_remove_body(it->second);
            lvl_.levelBodies_.erase(std::remove(lvl_.levelBodies_.begin(), lvl_.levelBodies_.end(), it->second), lvl_.levelBodies_.end());
            lvl_.entityBodies_.erase(it);
            ++dbgOv_.colliderRev_;
        }
#endif
    };
    h.spawnPrefab = [this](const fmt::OcPrefabInstance& pr, const Transform& local, scene::Entity parent) {
        prefabModel_.setContentDir(project_.contentDir());
        return prefabSys_.instantiate(pr.prefab, local, parent, pr.overrides, pr.name);
    };
    h.captureEntity = [this](scene::Entity e, fmt::OcWorldData& into, i32 parentIdx) {
        return captureEntityForInstance(e, into, parentIdx);
    };
    h.capturePrefabs = [this](scene::Entity root) {
        // The prefab instances under an edited instance, as the records its level file keeps.
        std::vector<fmt::OcPrefabInstance> out;
        scene::World& w = scene::World::instance();
        for (const scene::Entity r : prefabSys_.instanceRoots()) {
            bool under = false;
            for (scene::Entity p = w.parent(r); p != scene::kInvalidEntity; p = w.parent(p))
                if (p == root) { under = true; break; }
            if (!under) continue;
            fmt::OcPrefabInstance rec;
            rec.prefab = prefabSys_.prefabPathOf(r);
            const Transform& t = w.localTransform(r);
            rec.x = t.position.x; rec.y = t.position.y; rec.z = t.position.z;
            const Vec3 e = world::eulerDegFromQuat(t.rotation);
            rec.roll = e.x; rec.pitch = e.y; rec.yaw = e.z;
            rec.sx = t.scale.x; rec.sy = t.scale.y; rec.sz = t.scale.z;
            std::vector<fmt::OcPrefabOverride> ov;
            if (prefabSys_.computeOverrides(r, ov)) rec.overrides = std::move(ov);
            out.push_back(std::move(rec));
        }
        return out;
    };
#if AVER_MODULE_FRAMEWORK
    h.spawnClass = [](const fmt::OcWorldPlacement& p, const Transform& world, scene::Entity) {
        return game::spawnInstanceClass(p, world);
    };
    h.destroyClass = [](scene::Entity ce) { game::destroyInstanceClass(ce); };
#endif
    level_.setInstanceHooks(std::move(h));

#if AVER_MODULE_VOXI
    // Packed rows go through the foliage path: ray-traced geometry, resident while the instance is.
    game::LevelInstances::PackedSink ps;
    ps.set = [this](u64 owner, fmt::OcInstanceData rows, std::vector<std::string> mats) {
        if (rset_.voxiAttached_) levelFoliage_.setExtraTable(owner, std::move(rows), std::move(mats));
    };
    ps.clear = [this](u64 owner) { if (rset_.voxiAttached_) levelFoliage_.clearExtraTable(owner); };
    level_.setPackedSink(std::move(ps));
#endif
#if AVER_MODULE_FRAMEWORK
    level_.instances().onResidency = [](scene::Entity root, bool loaded, u32 count) {
        game::relayInstanceResidency(root, loaded, count);
    };
    // The framework's classes are declared by now or will be; deferred class placements spawn from spawnClassPlacements().
#endif

    // A child level cannot be rewritten (ids given on first use) while it is the open level with unsaved edits.
    level_.assets().setWriteGuard([this](const std::string& path, std::string* why) {
        const std::string open = world::LevelAssetLibrary::normalise(levelInstanceModel_.openLevel());
        if (!open.empty() && open == world::LevelAssetLibrary::normalise(path) && levelHasUnsavedEdits()) {
            if (why) *why = "save it first";
            return false;
        }
        return true;
    });

#ifdef AVER_WITH_LEVELINST_ABI
    // Scripts place, stream and query level instances through this seam.
    levelInstHost_ = game::makeAbiHost(level_.instances());
    aver_levelinst_set_host(&levelInstHost_);
#endif
}

editor::LevelInstanceUiCallbacks SandboxApp::levelInstanceUiCallbacks() {
    editor::LevelInstanceUiCallbacks cb;
    cb.pushUndo = [this](const editor::LevelInstanceEdit& e) { pushLevelInstanceEdit(e); };
    cb.select = [this](scene::Entity e) { selectionUi_.sel_ = SelectionState::kSelScene; selectionUi_.selEntity_ = e; };
    cb.markDirty = [this] { markLevelUnsaved(); };
    cb.status = [this](const std::string& s) { drawerUi_.cbStatus_ = s; };
    cb.openLevel = [this](const std::string& assetRef) { openInstanceLevel(assetRef); };
    cb.bakePack = [this](scene::Entity root, std::string* why) { return bakeInstancePack(root, why); };
    cb.removeReplaced = [this](const std::vector<scene::Entity>& es) {
        scene::World& w = scene::World::instance();
        for (const scene::Entity v : es) {
            if (!w.valid(v)) continue;
            EditCmd c = describeEntity(v);
            c.kind = EditCmd::Kind::Destroy;
            captureSubtree(c, v);
            destroyEntity(v);
            pushEdit(std::move(c));
        }
        multiClear();
    };
    return cb;
}

void SandboxApp::pushLevelInstanceEdit(const editor::LevelInstanceEdit& edit) {
    EditCmd c;
    c.kind = EditCmd::Kind::LevelInstance;
    c.label = edit.label;
    c.levelInst = std::make_shared<editor::LevelInstanceEdit>(edit);
    pushEdit(std::move(c));
}

// One live entity as the records of the level an instance is built from: a mesh placement, a light or a decal. Lights and
// decals are written in the edited instance's frame (the child level's own), not the world's.
bool SandboxApp::captureEntityForInstance(scene::Entity e, fmt::OcWorldData& into, i32 parentIdx) {
    scene::World& w = scene::World::instance();
    if (!w.valid(e) || w.destroyPending(e)) return false;
    if (w.component<scene::CMeshRenderer>(e, scene::kComponentMeshRenderer)) {
        fmt::OcWorldPlacement p;
        lvl_.capturingInstance_ = true;
        const bool ok = fillPlacementFromEntity(e, p, nullptr);
        lvl_.capturingInstance_ = false;
        if (!ok) return false;
        if (const auto it = lvl_.entityAuthoredName_.find(static_cast<u32>(e)); it != lvl_.entityAuthoredName_.end()) p.name = it->second;
        p.parent = parentIdx;
        into.placements.push_back(std::move(p));
        return true;
    }
    const scene::Entity root = level_.instances().rootOf(e);
    Transform frame = transformFromMatrix(w.worldMatrix(e));
    if (root != scene::kInvalidEntity)
        frame = transformFromMatrix(w.worldMatrix(e) * w.worldMatrix(root).inverse());
    if (const auto* lc = w.component<scene::CLight>(e, scene::kComponentLight)) {
        into.lights.push_back(editor::recordFromLight(*lc, frame, w.name(e), editor::LightAssetPaths::scan(project_.contentDir())));
        return true;
    }
    if (const auto* dc = w.component<scene::CDecal>(e, scene::kComponentDecal)) {
        if (dc->flags & scene::kDecalPooled) return false;
        into.decals.push_back(editor::recordFromDecal(*dc, frame, w.name(e), editor::LightAssetPaths::scan(project_.contentDir())));
        return true;
    }
    return false;
}

// Open Level: the instance's level on its own, with a way back.
void SandboxApp::openInstanceLevel(const std::string& assetRef) {
    const std::string full = levelInstanceModel_.fullPathFor(assetRef);
    editor::LevelReturnPoint rp;
    rp.levelPath = levelPath_;
    rp.camPos = viewState_.camPos_;
    rp.yawDeg = viewState_.yaw_ * kRadToDeg;
    rp.pitchDeg = viewState_.pitch_ * kRadToDeg;
    if (requestOpenLevel(full, "opened from a level instance")) levelInstanceModel_.pushReturn(std::move(rp));
    else drawerUi_.cbStatus_ = drawerUi_.openLevelError_;
}

void SandboxApp::levelInstanceBack() {
    editor::LevelReturnPoint rp;
    if (!levelInstanceModel_.popReturn(rp)) return;
    if (!requestOpenLevel(rp.levelPath, "back from a level instance")) drawerUi_.cbStatus_ = drawerUi_.openLevelError_;
}

bool SandboxApp::bakeInstancePack(scene::Entity root, std::string* why) {
    const std::string asset = level_.instances().assetOf(root);
    if (asset.empty()) { if (why) *why = "not an instance"; return false; }
    world::PackBakeReport rep;
    if (!game::bakeLevelPackWithContent(level_.assets(), asset, content_, project_.contentDir(), project_.dir, rep, why)) return false;
    drawerUi_.cbStatus_ = "Baked " + std::to_string(rep.rows) + " row(s) in " + std::to_string(rep.groups) + " group(s); " +
                std::to_string(rep.kept) + " stay entities";
    level_.instances().reloadAsset(asset);   // the bake changes what a packed instance shows
    return true;
}

// Ids stay on the placements that have them; once ANY placement has one, every placement gets one (instance overrides are
// keyed by them). A level nobody ever instanced and that never had an id round-trips byte for byte.
void SandboxApp::keepPlacementIds(fmt::OcWorldData& w, const std::unordered_map<u32, i32>& slotOf) {
    bool any = false;
    for (const fmt::OcWorldPlacement& p : w.placements) if (p.placementId) { any = true; break; }
    if (!any) return;
    fmt::assignPlacementIds(w);
    // The ids just made are remembered, so the next save writes the same ones whatever moved in between.
    for (const auto& [key, slot] : slotOf)
        if (slot >= 0 && static_cast<usize>(slot) < w.placements.size() && w.placements[static_cast<usize>(slot)].placementId)
            lvl_.entityPlaceId_[key] = w.placements[static_cast<usize>(slot)].placementId;
}

// EXTENT: the union of the placements' world boxes (and the instances' bounds), the level's own frame.
void SandboxApp::writeLevelExtent(fmt::OcWorldData& w) {
    bool any = false;
    f32 mn[3] = {0, 0, 0}, mx[3] = {0, 0, 0};
    const auto widen = [&](const f32 lo[3], const f32 hi[3]) {
        for (int a = 0; a < 3; ++a) {
            if (!any) { mn[a] = lo[a]; mx[a] = hi[a]; }
            else { mn[a] = std::min(mn[a], lo[a]); mx[a] = std::max(mx[a], hi[a]); }
        }
        any = true;
    };
    if (level_.streaming().active()) {
        for (const fmt::OcWorldPlacement& p : w.placements)
            if (p.hasBounds) widen(p.boundsMin, p.boundsMax);
    } else {
        scene::World& world = scene::World::instance();
        for (const scene::Entity e : lvl_.levelEntities_) {
            if (!world.valid(e) || level_.instances().isMember(e)) continue;
            f32 lo[3], hi[3];
            if (entityWorldBounds(e, lo, hi)) widen(lo, hi);
        }
    }
    Vec3 ilo, ihi;
    if (level_.instances().extentBounds(ilo, ihi)) {
        const f32 lo[3] = {ilo.x, ilo.y, ilo.z}, hi[3] = {ihi.x, ihi.y, ihi.z};
        widen(lo, hi);
    }
    w.hasExtent = any;
    for (int a = 0; a < 3; ++a) { w.extentMin[a] = any ? mn[a] : 0.0; w.extentMax[a] = any ? mx[a] : 0.0; }
}

} // namespace aver

#endif // AVER_MODULE_SCENE
