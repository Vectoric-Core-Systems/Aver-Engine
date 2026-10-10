// The editor's side of level streaming (docs/LEVEL_STREAMING.md): streamed entities join the level's own
// bookkeeping while resident, write their edits back into the placement record before they leave, and
// a save writes every record plus the generated .ocstream under Binaries/Streaming.
#include "SandboxApp.hpp"

#if AVER_MODULE_SCENE
#  include "aver/game/LevelStreaming.hpp"
#  include "aver/formats/OcStream.hpp"
#  include "aver/scene/scene_abi.h"
#  include "aver/world/LevelTransform.hpp"
#  if AVER_MODULE_PHYSICS
#    include "aver/physics/physics_abi.h"
#  endif

#  include <algorithm>
#  include <filesystem>
#  include <set>
#  include <unordered_set>

namespace aver {

void SandboxApp::installLevelStreamHooks(Engine& eng) {
    level_.setDevice(eng.device());
    game::LevelStreaming::Hooks h;
    h.afterLoad = [this](const std::vector<scene::Entity>& es, const std::vector<u32>& placements,
                         const std::vector<i32>& bodies) { onStreamedIn(es, placements, bodies); };
    h.beforeEvict = [this](scene::Entity e, u32 placement, bool writeBack) { onStreamedOut(e, placement, writeBack); };
    level_.setStreamHooks(std::move(h));
}

void SandboxApp::onStreamedIn(const std::vector<scene::Entity>& es, const std::vector<u32>& placements,
                              const std::vector<i32>& bodies) {
    const auto& records = level_.streaming().placements();
    for (usize k = 0; k < es.size(); ++k) {
        const scene::Entity e = es[k];
        const fmt::OcWorldPlacement& p = records[placements[k]];
        const u32 key = static_cast<u32>(e);
        levelEntities_.push_back(e);
        const std::string label = p.name.empty() ? makeEntityLabel(p.material, p.asset) : p.name;
        entityLabels_[key] = label;
        streamLabel_[key] = label;
        entityCollide_[key] = p.collide;
        if (!p.animClip.empty()) entityAnim_[key] = EntityAnim{p.animClip, p.animSpeed, p.animTime, p.animOnce};
        if (p.snapToGround) entitySnapZ_[key] = p.z;
#  if AVER_MODULE_PHYSICS
        if (k < bodies.size() && bodies[k] >= 0) entityBodies_[key] = bodies[k];
#  else
        (void)bodies;
#  endif
    }
    streamSeenCount_ = levelEntities_.size();
}

void SandboxApp::onStreamedOut(scene::Entity e, u32 placement, bool writeBack) {
    const u32 key = static_cast<u32>(e);
    // A Play session's moves are not edits: the record keeps what the level said.
    if (writeBack && !anyPlayActive()) {
        fmt::OcWorldPlacement p;
        if (fillPlacementFromEntity(e, p, nullptr)) refreshStreamRecord(e, placement, p);
    }
#  if AVER_MODULE_PHYSICS
    // The body the editor holds now (an edit rebuilds it); LevelStreaming removes the one it made.
    if (const auto b = entityBodies_.find(key); b != entityBodies_.end()) {
        aver_phys_remove_body(b->second);
        levelBodies_.erase(std::remove(levelBodies_.begin(), levelBodies_.end(), b->second), levelBodies_.end());
    }
#  endif
    levelEntities_.erase(std::remove(levelEntities_.begin(), levelEntities_.end(), e), levelEntities_.end());
    streamSeenCount_ = levelEntities_.size();
    entityLabels_.erase(key);
    streamLabel_.erase(key);
    entityCollide_.erase(key);
    entityAnim_.erase(key);
    entitySnapZ_.erase(key);
    entityBodies_.erase(key);
    streamPinned_.erase(key);
    streamSelPinned_.erase(key);
}

// A placement record from a live entity: parented as the record says while that still holds, else at
// its world pose as a root (a reparented or newly made child).
bool SandboxApp::streamRecordFromEntity(scene::Entity e, const std::unordered_map<u32, const Transform*>* animPlaced,
                                        fmt::OcWorldPlacement& p, bool& keepParent) {
    if (!fillPlacementFromEntity(e, p, animPlaced)) return false;
    game::LevelStreaming& ls = level_.streaming();
    scene::World& world = scene::World::instance();
    const scene::Entity livePar = world.parent(e);
    const fmt::OcWorldPlacement* old = ls.recordOf(e);
    const scene::Entity recPar = old && old->parent >= 0 ? ls.entityOf(static_cast<u32>(old->parent)) : scene::kInvalidEntity;
    keepParent = livePar == recPar;
    if (!keepParent || (!old && livePar != scene::kInvalidEntity)) {
        const Transform wt = transformFromMatrix(world.worldMatrix(e));
        p.x = wt.position.x; p.y = wt.position.y; p.z = wt.position.z;
        const Vec3 eu = world::eulerDegFromQuat(wt.rotation);
        p.roll = eu.x; p.pitch = eu.y; p.yaw = eu.z;
        p.sx = wt.scale.x; p.sy = wt.scale.y; p.sz = wt.scale.z;
        p.snapToGround = false;
        p.parent = -1;
        keepParent = false;
    }
    return true;
}

// The record keeps what the entity cannot say (its id, its object id), and its stored name unless the
// outliner label was changed since it streamed in.
void SandboxApp::refreshStreamRecord(scene::Entity e, u32 placement, fmt::OcWorldPlacement p) {
    game::LevelStreaming& ls = level_.streaming();
    const fmt::OcWorldPlacement& old = ls.placements()[placement];
    bool keepParent = true;
    fmt::OcWorldPlacement fresh;
    if (streamRecordFromEntity(e, nullptr, fresh, keepParent) && !keepParent) p = fresh;
    p.placementId = old.placementId;
    p.objectId = old.objectId;
    p.name = old.name;
    p.hasBounds = old.hasBounds;
    for (int i = 0; i < 3; ++i) { p.boundsMin[i] = old.boundsMin[i]; p.boundsMax[i] = old.boundsMax[i]; }
    const auto label = entityLabels_.find(static_cast<u32>(e));
    const auto was = streamLabel_.find(static_cast<u32>(e));
    if (label != entityLabels_.end() && (was == streamLabel_.end() || label->second != was->second)) p.name = label->second;
    ls.updateRecord(placement, p, keepParent);
}

void SandboxApp::pinStreamed(scene::Entity e) {
    game::LevelStreaming& ls = level_.streaming();
    if (!ls.active() || !streamPinned_.insert(static_cast<u32>(e)).second) return;
    const i32 pi = ls.placementOf(e);
    if (pi >= 0) ls.pin(static_cast<u32>(pi), true);
}

void SandboxApp::onStreamedEntityDestroyed(scene::Entity e) {
    game::LevelStreaming& ls = level_.streaming();
    if (!ls.active()) return;
    const i32 pi = ls.placementOf(e);
    if (pi >= 0) ls.removeRecord(static_cast<u32>(pi));
    streamAdopted_.erase(static_cast<u32>(e));
}

// Entities made this session (drop, duplicate, paste, an undone delete) become records at once, so they
// hold their mesh and save like any other placement.
void SandboxApp::adoptNewLevelEntities() {
    game::LevelStreaming& ls = level_.streaming();
    if (levelEntities_.size() == streamSeenCount_) return;
    streamSeenCount_ = levelEntities_.size();
    scene::World& world = scene::World::instance();
    const std::vector<scene::Entity> live = levelEntities_;
    for (const scene::Entity e : live) {
        if (!world.valid(e) || ls.owns(e) || !streamAdopted_.insert(static_cast<u32>(e)).second) continue;
        fmt::OcWorldPlacement p;
        bool keepParent = false;
        if (!streamRecordFromEntity(e, nullptr, p, keepParent)) continue;   // a light, decal or prefab part
        const auto label = entityLabels_.find(static_cast<u32>(e));
        if (label != entityLabels_.end()) { p.name = label->second; streamLabel_[static_cast<u32>(e)] = label->second; }
        p.objectId = fnv1a64(std::string_view(p.asset));
        ls.addRecord(p, e);
        streamPinned_.insert(static_cast<u32>(e));
    }
}

void SandboxApp::tickLevelStreaming(f32 dt) {
    std::vector<Vec3> viewers{camPos_};
    game::LevelStreaming& ls = level_.streaming();
    if (ls.active()) {
        adoptNewLevelEntities();
        // What is selected stays loaded while it is selected, so a gesture never loses its entity.
        std::unordered_set<u32> selected;
        if (selEntity_ != scene::kInvalidEntity) selected.insert(static_cast<u32>(selEntity_));
        for (const scene::Entity e : multiSel_) selected.insert(static_cast<u32>(e));
        // An object with a sequence track stays loaded too (the track holds its entity).
        for (const scene::Entity e : seqEditor_.trackEntities()) pinStreamed(e);
        for (const u32 key : selected) {
            if (!streamSelPinned_.insert(key).second) continue;
            const i32 pi = ls.placementOf(static_cast<scene::Entity>(key));
            if (pi >= 0) ls.pin(static_cast<u32>(pi), true);
        }
        for (auto it = streamSelPinned_.begin(); it != streamSelPinned_.end();) {
            if (selected.count(*it)) { ++it; continue; }
            const i32 pi = ls.placementOf(static_cast<scene::Entity>(*it));
            if (pi >= 0 && !streamPinned_.count(*it)) ls.pin(static_cast<u32>(pi), false);
            it = streamSelPinned_.erase(it);
        }
        // Play's own moves end with Play: what streamed in during it reloads from its record.
        const bool playing = anyPlayActive();
        if (playing && !streamPlayWas_) {
            streamPlayResident_.clear();
            for (const scene::Entity e : levelEntities_) if (ls.owns(e)) streamPlayResident_.insert(static_cast<u32>(e));
        } else if (!playing && streamPlayWas_) {
            std::vector<scene::Entity> fresh;
            for (const scene::Entity e : levelEntities_)
                if (ls.owns(e) && !streamPlayResident_.count(static_cast<u32>(e))) fresh.push_back(e);
            if (!fresh.empty()) ls.reload(scene::World::instance(), fresh);
            streamPlayResident_.clear();
        }
        streamPlayWas_ = playing;
    }
    level_.tickStreaming(viewers);
#  if AVER_MODULE_VOXI
    if (voxiAttached_) levelFoliage_.update(camPos_, dt);
#  else
    (void)dt;
#  endif
}

// Before a save: resident records take their entity's state, and every record (resident or not) is
// written. `slotOf` maps each resident entity to its line.
void SandboxApp::buildStreamedPlacements(fmt::OcWorldData& w,
                                         const std::unordered_map<u32, const Transform*>& animPlaced,
                                         std::unordered_map<u32, i32>& slotOf) {
    game::LevelStreaming& ls = level_.streaming();
    adoptNewLevelEntities();
    scene::World& world = scene::World::instance();
    for (const scene::Entity e : levelEntities_) {
        if (!world.valid(e)) continue;
        const i32 pi = ls.placementOf(e);
        fmt::OcWorldPlacement p;
        if (pi < 0 || !fillPlacementFromEntity(e, p, &animPlaced)) continue;
        refreshStreamRecord(e, static_cast<u32>(pi), p);
    }
    const auto& records = ls.placements();
    streamOutSlot_.assign(records.size(), -1);
    for (u32 i = 0; i < records.size(); ++i) {
        // Class placements are written by appendClassPlacements, from their live instances.
        if (ls.removed(i) || !records[i].className.empty()) continue;
        fmt::OcWorldPlacement p = records[i];
        p.parent = p.parent >= 0 && static_cast<usize>(p.parent) < streamOutSlot_.size()
                 ? streamOutSlot_[static_cast<usize>(p.parent)] : -1;
        streamOutSlot_[i] = static_cast<i32>(w.placements.size());
        w.placements.push_back(std::move(p));
    }
    for (const scene::Entity e : levelEntities_) {
        const i32 pi = ls.placementOf(e);
        if (pi >= 0 && streamOutSlot_[static_cast<usize>(pi)] >= 0)
            slotOf[static_cast<u32>(e)] = streamOutSlot_[static_cast<usize>(pi)];
    }
}

// A streamed level's sequence tracks on objects that were not loaded when it opened: kept aside at load
// and written back here, renumbered to where their placements were saved.
void SandboxApp::appendStreamedSequenceTracks(fmt::OcWorldData& w) {
    if (streamSeqTracks_.empty()) return;
    if (w.sequences.empty()) w.sequences.push_back(streamSeqHeader_);
    for (fmt::OcSeqTrack tr : streamSeqTracks_) {
        if (tr.target < 0 || static_cast<usize>(tr.target) >= streamOutSlot_.size() || streamOutSlot_[static_cast<usize>(tr.target)] < 0)
            continue;   // its placement was deleted
        tr.target = streamOutSlot_[static_cast<usize>(tr.target)];
        w.sequences.front().tracks.push_back(std::move(tr));
    }
}

// World bounds of a live entity's mesh through its world matrix (row vectors, translation in row 3).
bool SandboxApp::entityWorldBounds(scene::Entity e, f32 outMin[3], f32 outMax[3]) {
    scene::World& world = scene::World::instance();
    const auto* mr = world.component<scene::CMeshRenderer>(e, scene::kComponentMeshRenderer);
    const std::pair<Vec3, Vec3>* lb = mr ? content_.boundsFor(mr->mesh) : nullptr;
    if (!lb) return false;
    const Mat4& m = world.worldMatrix(e);
    for (int a = 0; a < 3; ++a) { outMin[a] = 1e30f; outMax[a] = -1e30f; }
    for (int c = 0; c < 8; ++c) {
        const f32 p[3] = {(c & 1) ? lb->second.x : lb->first.x, (c & 2) ? lb->second.y : lb->first.y,
                          (c & 4) ? lb->second.z : lb->first.z};
        for (int a = 0; a < 3; ++a) {
            const f32 v = p[0] * m.m[0][a] + p[1] * m.m[1][a] + p[2] * m.m[2][a] + m.m[3][a];
            outMin[a] = std::min(outMin[a], v);
            outMax[a] = std::max(outMax[a], v);
        }
    }
    return true;
}

// A save of a level World Settings sets to stream: ids and world bounds (a whole-loaded level has every
// mesh at hand; a streamed one has them on its records), then the generated .ocstream.
void SandboxApp::stampStreamFields(fmt::OcWorldData& w, const std::unordered_map<u32, i32>& slotOf) {
    if (!w.stream.enabled) return;
    if (!level_.streaming().active()) {
        std::vector<scene::Entity> bySlot(w.placements.size(), scene::kInvalidEntity);
        for (const auto& [key, slot] : slotOf)
            if (slot >= 0 && static_cast<usize>(slot) < bySlot.size())
                bySlot[static_cast<usize>(slot)] = static_cast<scene::Entity>(key);
        for (usize i = 0; i < w.placements.size(); ++i)
            if (bySlot[i] != scene::kInvalidEntity)
                w.placements[i].hasBounds = entityWorldBounds(bySlot[i], w.placements[i].boundsMin, w.placements[i].boundsMax);
    }
    fmt::assignPlacementIds(w);
    std::string why;
    const std::string stem = std::filesystem::path(levelPath_).stem().string();
    if (!fmt::writeOcStreamFor(w, stem, project_.dir, level_.streamFoliage(), &why))
        AVER_WARN("[LevelStream] could not write the streaming data: {}", why);
}

// World Settings > Regenerate Streaming Data: saves, then rebuilds every placement's bounds from its
// mesh and cuts the foliage into cells (fmt::bakeOcStream), and reopens the level to use it.
bool SandboxApp::regenerateStreamData(std::string& report) {
    if (!saveLevel(levelPath_)) { report = "the level could not be saved"; return false; }
    fmt::OcWorldData w;
    std::string why;
    if (!fmt::loadOcworld(levelPath_, w, &why)) { report = why; return false; }
    w.stream.enabled = true;
    fmt::OcStreamBakeOptions opt;
    opt.foliageCellCm = w.stream.cellCm > 0.0f ? w.stream.cellCm : 6400.0f;
    opt.meshBounds = [this](const std::string& asset, Vec3& lo, Vec3& hi) {
        const std::pair<Vec3, Vec3>* b = content_.boundsFor(fnv1a64(std::string_view(asset)));
        if (!b) return false;
        lo = b->first;
        hi = b->second;
        return true;
    };
    fmt::OcStreamBakeReport rep;
    const std::string stem = std::filesystem::path(levelPath_).stem().string();
    if (!fmt::bakeOcStream(w, stem, project_.contentDir(), project_.dir, opt, rep, &why) ||
        !fmt::saveOcworld(levelPath_, w, &why)) {
        report = why;
        return false;
    }
    levelHeader_.stream = w.stream;
    markLevelSaved();
    report = std::to_string(rep.withBounds) + " of " + std::to_string(rep.placements) + " objects with bounds, " +
             std::to_string(rep.foliageInstances) + " foliage instances in " + std::to_string(rep.foliageCells) + " cells";
    AVER_INFO("[LevelStream] regenerated {}: {}", w.stream.dataPath, report);
    requestOpenLevel(levelPath_, "reopened with regenerated streaming data");
    return true;
}

// World Settings > Level Streaming.
void SandboxApp::buildLevelStreamingSettings() {
    ImGui::TextDisabled("Level Streaming");
    ImGui::Separator();
    fmt::OcStreamSettings& st = levelHeader_.stream;
    bool on = st.enabled;
    if (ImGui::Checkbox("Stream this level", &on)) {
        st.enabled = on;
        markLevelUnsaved();
    }
    uiReg_.track("worldSettings.levelStreaming");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Load the level's objects, meshes and foliage by distance from the camera\n"
                          "instead of all at once. Objects stay editable; saving writes every one.");
    if (st.enabled) {
        f32 cellM = st.cellCm / 100.0f, loadM = st.loadCm / 100.0f, evictM = st.evictCm / 100.0f;
        ImGui::SetNextItemWidth(200.0f * dpi_);
        if (ImGui::SliderFloat("Cell size (m)", &cellM, 16.0f, 256.0f, "%.0f")) {
            st.cellCm = cellM * 100.0f;
            markLevelUnsaved();
        }
        ImGui::SetNextItemWidth(200.0f * dpi_);
        if (ImGui::SliderFloat("Load distance (m)", &loadM, 50.0f, 2000.0f, "%.0f")) {
            st.loadCm = loadM * 100.0f;
            if (st.evictCm < st.loadCm + 1000.0f) st.evictCm = st.loadCm + 1000.0f;
            markLevelUnsaved();
        }
        ImGui::SetNextItemWidth(200.0f * dpi_);
        if (ImGui::SliderFloat("Unload distance (m)", &evictM, loadM + 10.0f, 2500.0f, "%.0f")) {
            st.evictCm = evictM * 100.0f;
            markLevelUnsaved();
        }
        ImGui::TextDisabled("Objects load inside the load distance and unload past the unload distance.");
        ImGui::TextDisabled("Data: %s", st.dataPath.empty() ? "(not generated yet)" : st.dataPath.c_str());
        if (level_.streaming().active() && level_.streamDataStale())
            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "Streaming data is out of date.");
        if (ImGui::Button("Regenerate Streaming Data")) {
            std::string report;
            streamRegenReport_ = regenerateStreamData(report) ? report : "Failed: " + report;
        }
        uiReg_.track("worldSettings.regenerateStreamData");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Saves the level, rebuilds every object's bounds from its mesh and sorts the\n"
                              "foliage into cells, into Binaries/Streaming, then reopens the level.\n"
                              "Saving keeps it current; use this after reimporting meshes or foliage.");
        if (!streamRegenReport_.empty()) ImGui::TextDisabled("%s", streamRegenReport_.c_str());
    }
    const game::LevelStreaming& ls = level_.streaming();
    if (ls.active()) {
        ImGui::Text("Loaded: %zu of %zu objects (%zu entities)", ls.residentRoots(), ls.rootCount(),
                    ls.residentEntities());
#  if AVER_MODULE_VOXI
        if (levelFoliage_.streamed())
            ImGui::Text("Foliage: %u of %u cells, %u instances", levelFoliage_.residentCells(),
                        levelFoliage_.totalCells(), levelFoliage_.instances());
#  endif
    }
    if (st.enabled != ls.active()) {
        ImGui::TextDisabled(st.enabled ? "Starts when the level is reopened." : "Stops when the level is reopened.");
        if (ImGui::Button("Save and Reopen")) {
            if (saveLevel(levelPath_)) {
                markLevelSaved();
                requestOpenLevel(levelPath_, "reopened to apply level streaming");
            }
        }
        uiReg_.track("worldSettings.streamReopen");
        ImGui::TextDisabled("Meshes load on demand from the next project open.");
    }
}

} // namespace aver

#endif // AVER_MODULE_SCENE
