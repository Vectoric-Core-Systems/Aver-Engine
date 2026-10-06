// Runtime side: level load and unload, sky, landscape and water application, chunk streaming, navigation for a level.
// Part of SandboxApp, split out of the single 29,952-line SandboxApp.cpp on 2026-09-16 by moving method bodies
// verbatim; the class itself is declared in SandboxApp.hpp.

#include "SandboxApp.hpp"
#include "LightLevelIo.hpp"
// applyLevelSky below calls assets::applyLevelEnv, and the only thing that brings that declaration
// in is SandboxApp.hpp's AVER_MODULE_PBR include block -- so a PBR-off build lost the header while
// the call, guarded on AVER_MODULE_SCENE, stayed compiled in. Named here rather than left to the
// header because the dependency is honest at both ends: LevelSky.hpp is header-only over
// Aver.Core/Aver.Formats/Aver.RHI and names no pbr:: type at all (its own header comment argues
// that a build without the renderer STILL has an rhi::SkyAtmosphere to fill), and Aver.Formats --
// which Sandbox links unconditionally -- puts modules/assets/include on the public include path
// through its Aver.Assets dependency, so this resolves with PBR on or off. #pragma once makes the
// second arrival a no-op, so the default build sees exactly what it saw before.
#include "aver/assets/LevelSky.hpp"
// loadLevel's afterInstantiate hook loads the level's instanced foliage once placements exist --
// see the header for the contract three packages implement parts of.
#if AVER_MODULE_VOXI
#include "aver/game/GameFoliage.hpp"
#endif

namespace aver {
#if AVER_MODULE_SYNAPSE
// Loads the navigation that belongs to a level, if it has any. An ABSENT file is the normal
// case -- every level until somebody bakes one -- so it clears rather than complains.
void SandboxApp::loadNavForLevel(Engine& e) {
    nav_ = fmt::OcNavData{};
    const std::string path = editor::navPathForLevel(levelPath_);
    std::string why;
    if (!path.empty() && fmt::loadOcNav(path, nav_, &why)) {
        AVER_INFO("[Editor] navigation loaded from {} ({}x{} cells)", path,
                  nav_.widthCells, nav_.heightCells);
    }
    rebuildNavOverlay(e);
}

#endif

void SandboxApp::setFocusCompile(bool b) { tools_.armCompile(b); }

void SandboxApp::setFocusReload(int frames) { if (frames > 0) tools_.armReload(frames); }

#if AVER_MODULE_LANDSCAPE
// Safe because the generator lives inside streaming_, which this object owns and destroys.
void SandboxApp::applyLandscapeToStreaming() {
#if AVER_MODULE_SCENE
    // Nothing streaming yet: setChunkStreamingEnabled wires the source itself when it opens, so
    // the common order (level loads terrain, streaming switched on afterwards) needs nothing here.
    if (!streaming_.enabled()) return;
    // ChunkWorld exposes settings() as CONST ONLY -- no supported way to swap a live generator's
    // height source, and adding one would widen that module's API just for the editor's
    // convenience. Restarting streaming re-opens through the same known path and re-generates only
    // chunks around the camera, also dropping every chunk generated against the OLD surface (a known gap around generatorVersion).
    AVER_INFO("[ChunkWorld] terrain changed under a live stream -- restarting it so scatter "
              "re-generates against the new surface");
    setChunkStreamingEnabled(false);
    setChunkStreamingEnabled(true);
#endif
}

#endif

#if AVER_MODULE_SCENE
#if AVER_MODULE_SCENE
// True when any resident density field owns `e`. The World Outliner and the save path both use
// this to tell streamed entities from authored ones, so it MUST see every world -- a streamed
// entity that no world claims would be offered for editing and written into the level file.
bool SandboxApp::anyChunkWorldOwns(scene::Entity e) const {
    return streaming_.owns(e);
}

#endif
#endif

#if AVER_MODULE_SCENE
// Turns chunk streaming on or off around the editor camera. game::GameStreaming::enable builds one
// aver::world::ChunkWorld per non-"Sky" PCGVOLUME the level declares (its own comment has the full
// one-world-per-density-field reasoning); this just supplies what it cannot reach on its own: the
// project, the content cache, and a height source built from whichever terrain is resident.
// levelPcgVolumes_/levelHeader_.scatterSpecies, NOT level_.pcgVolumes(): those are the editor's own
// EDITABLE copies, so a World Settings page edit reaches the next enable.
void SandboxApp::setChunkStreamingEnabled(bool on) {
    if (on == streaming_.enabled()) return;

    if (!on) {
        streaming_.disable();
        return;
    }

    if (!project_.valid()) {
        AVER_WARN("[ChunkWorld] cannot enable streaming: no project is open");
        return;
    }

    // Scatter follows the terrain when a section is resident: the generator asks only "what is the
    // surface Z at (x, y)" and knows nothing about landscapes, so the editor (which depends on
    // both) closes this lambda. The authored section wins where it exists; past its rim this falls
    // through to the same continuous noise the ring tiles use.
    game::GameStreaming::HeightQueryFn height;
#if AVER_MODULE_LANDSCAPE
    if (landscape_.loaded())
        height = [this](f32 x, f32 y, f32& outZ) { return landscape_.scatterHeightAt(x, y, outZ); };
#endif

    streaming_.enable(project_, levelPcgVolumes_, levelHeader_.scatterSpecies, &content_,
                       std::move(height));
    if (streaming_.enabled())
        streaming_.warnIfCameraOutsideGeneratedBand(
            camPos_, "press F to focus something near ground level, or fly down.");
}

// Spawns (or despawns) the graph-driven drone. OPT-IN, same shape as setChunkStreamingEnabled:
// Window > Drone or --drone, nothing touched until asked for.
// TRANSIENT, LIKE A CHUNK-STREAMED ENTITY, ON PURPOSE: droneEntity_ is never pushed to
// levelEntities_, so saveLevel/undo/redo never see it, and buildPanels' World Outliner filters it
// out explicitly by entity id, the same way it filters streaming_.owns(e).
void SandboxApp::setDroneEnabled(bool on) {
    if (on == (droneEntity_ != scene::kInvalidEntity)) return;

    if (!on) {
        scene::World& world = scene::World::instance();
        if (world.valid(droneEntity_)) { world.destroy(droneEntity_); world.flush(); }
#if AVER_MODULE_SCRIPTING
        if (scripts_.ready()) scripts_.graphUnload(static_cast<i32>(droneEntity_));
#endif
        AVER_INFO("[Drone] disabled -- entity #{} released", (u32)droneEntity_);
        droneEntity_ = scene::kInvalidEntity;
        droneGraphLoaded_ = false;
        droneHaveLastPos_ = false;
        return;
    }

    if (!project_.valid()) {
        AVER_WARN("[Drone] cannot enable: no project is open");
        return;
    }
#if AVER_MODULE_SCRIPTING
    if (!scripts_.ready()) {
        AVER_WARN("[Drone] cannot enable: the scripting host is not running ({})",
                  scripts_.declineReason());
        return;
    }
    if (!scripts_.graphAvailable()) {
        AVER_WARN("[Drone] cannot enable: this build's staged bridge exports no Graph entry "
                  "points -- rebuild with the .NET SDK present so Aver.Scripting.Bridge picks up "
                  "GraphLoad/GraphTick/GraphUnload");
        return;
    }
#else
    AVER_WARN("[Drone] cannot enable: this build has no scripting module (AVER_MODULE_SCRIPTING=OFF)");
    return;
#endif
    scene::World& world = scene::World::instance();
    Transform xf;
    // AT THE PLAYER START WHEN THE LEVEL HAS ONE: the drone is what flies when a project declares
    // no GameMode, so it IS the player for that session, and that's exactly what a Player Start
    // says. Falling back to the camera keeps a level with no marker behaving as before.
    {
        Vec3 sp{}; f32 sy = 0.0f;
        if (playerStartTransform(sp, sy)) {
            xf.position = sp;
            xf.rotation = quatFromEulerDeg(Vec3{0.0f, 0.0f, sy});
            AVER_INFO("[Drone] spawning at the level's Player Start ({:.0f}, {:.0f}, {:.0f})",
                      sp.x, sp.y, sp.z);
        } else {
            xf.position = camPos_ + camForward() * kAddDistance;
            xf.rotation = Quat{0, 0, 0, 1};
        }
    }
    // 50cm half-extent applied to a mesh normalised so its rotor-tip diagonal reach is exactly
    // 1.0 gives a HUB-to-hub diagonal span of 2*0.80*50cm = 80cm, a prop-tip-to-prop-tip diagonal
    // of 2*1.00*50cm = 100cm, and a straight footprint of about 2*0.7657*50cm ~= 76.6cm -- a
    // mid-size camera/mapping drone, not a car (400-500cm) or a toy (25-40cm hub-to-hub):
    // comfortably "drone-sized".
    // Reusing kEditorCubeHalf rather than a second size dial: the SAME "how big does a built-in primitive spawn" constant the cube used.
    xf.scale = Vec3{kEditorCubeHalf, kEditorCubeHalf, kEditorCubeHalf};

    // FROZEN, same as spawnCube: the entity name is the asset path the mesh resolver hashes. Was
    // "Meshes/cube.ocmesh" -- a graph-driven actor spawned as a literal unit cube, the entire "the
    // drone has a box" complaint appendDrone exists to fix (see GameContent.cpp's runtime copy).
    static const std::string kDroneAsset = "Meshes/drone.ocmesh";
    const scene::Entity e = world.create(kDroneAsset, scene::kInvalidEntity, xf);
    if (e == scene::kInvalidEntity) { AVER_WARN("[Drone] the world refused a new entity"); return; }
    if (auto* mr = static_cast<scene::CMeshRenderer*>(
            world.addComponent(e, scene::kComponentMeshRenderer))) {
        mr->mesh = fnv1a64(std::string_view(kDroneAsset));
        mr->flags |= scene::kMeshRendererVisible;
        // The unit CUBE's box, not the drone's -- left over from when kDroneAsset really was the
        // cube. appendDrone's flat, spread-out silhouette needs its own bounds, or the drone ends
        // up exempt from frustum culling in the wrong direction: a box roughly six times taller
        // than the mesh it bounds -- the same failure mode inverted (too loose instead of absent).
        mr->aabbMin[0] = mr->aabbMin[1] = -0.78f; mr->aabbMin[2] = -0.22f;
        mr->aabbMax[0] = mr->aabbMax[1] =  0.78f; mr->aabbMax[2] =  0.16f;
        // A sensible built-in look for an unpainted quadcopter chassis: greyish and mostly metal
        // (the M_Metal entry in content_'s SurfaceLook table) rather than the flat grey
        // 0.80/0.80/0.85 fallback an unset material draws.
        mr->material = aver_scene_material(0, "M_Metal");
    }
    // Deliberately NOT levelEntities_.push_back(e) and NOT pushEdit(...): see the comment above
    // this function for why.

#if AVER_MODULE_SCRIPTING
    // The graph comes from the PROJECT, named by --drone-graph; the engine has no opinion about
    // what it's called.
    // It used to be hardcoded as Content\Scripts\Drone.ocgraph, assuming every project contained
    // that file -- one sample's content compiled into the editor.
    // THE PROJECT'S OWN ANSWER FIRST, then the CLI override: until DRONE.GRAPH existed the drone
    // spawned with no graph from anywhere but --drone-graph, sitting perfectly still.
    const std::string& droneRel = !droneGraphRel_.empty() ? droneGraphRel_ : project_.droneGraph;
    const std::string graphPath =
        droneRel.empty() ? std::string() : project_.contentDir() + "\\" + droneRel;
    if (graphPath.empty()) {
        AVER_WARN("[Drone] entity #{} spawned with NO graph, so it will sit still. Give the "
                  "project a DRONE.GRAPH line (e.g. `DRONE.GRAPH Scripts/Drone.ocgraph`), or "
                  "pass --drone-graph <path relative to Content>.",
                  (u32)e);
        droneGraphLoaded_ = false;
    } else {
        droneGraphLoaded_ = scripts_.graphLoad(static_cast<i32>(e), graphPath);
        if (!droneGraphLoaded_)
            AVER_WARN("[Drone] entity #{} spawned but its graph would not load from '{}' -- see "
                      "the [Graph] error line just above for why", (u32)e, graphPath);
    }
#endif
    droneEntity_ = e;
    droneTimeSeconds_ = 0.0f;
    droneHaveLastPos_ = false;
    droneLogsLeft_ = 30;
    AVER_INFO("[Drone] enabled -- entity #{} spawned at ({:.0f},{:.0f},{:.0f}), graph {}",
              (u32)e, xf.position.x, xf.position.y, xf.position.z,
              droneGraphLoaded_ ? "loaded" : "NOT loaded");
}

// Sum of triangle counts over every entity streaming_ currently owns. O(residentEntities), walked
// fresh each call rather than kept running -- a few hundred at most, and this only runs while the
// streaming panel is open or a log line needs it.
u64 SandboxApp::residentTriangleCount() const {
    return streaming_.residentTriangleCount([this](u64 id) -> u32 {
        const auto it = meshTris_.find(id);
        return it != meshTris_.end() ? it->second : 0u;
    });
}

// Loads a level file into the world as ordinary scene entities: transform, mesh and name.
// TAKES Engine& so it can hand a real device down to landscape_.loadForLevel: unloadLevel/
// landscape_.unload need a device to free what the PREVIOUS level left resident, and the only
// device this function ever has is the one its own two callers already hold.
//
// THE PARSE AND THE PLACEMENT LOOP ARE level_'s, the runtime's GameLevel, so the editor and a shipped
// game read a level with the same code. GameLevel DISPATCHES ON WHAT THE FILE ACTUALLY USES, NOT ITS
// EXTENSION OR HEADER LINE: ElectricDreams and FirstPerson both ship an .ocmap starting with `OCMAP 1`
// that is nonetheless pure OCWORLD content, while OpenConstructor's demoworld.ocmap genuinely needs the
// legacy grammar. Without that, every level went through fmt::loadOcworld, which "succeeds" on legacy
// OCMAP while silently skipping ROOT/CLIENT/SURFACE/GROUND/KILLZ/DEFORM -- opening and saving
// demoworld.ocmap destroyed all six records with no warning.
//
// What the editor does around the load enters through level_'s hooks: the header, water, sky field and
// terrain before the placements (an OCWORLD file only), and its own per-entity records after
// (onLevelInstantiated, onLegacyOcmapInstantiated). A file that does not parse fires neither, and
// level_ logs why.
void SandboxApp::loadLevel(Engine& eng, const std::string& path) {
    unloadLevel(eng);

    game::GameLevel::LoadHooks hooks;
    hooks.beforePlacements = [this, &eng](const std::string& levelPath, const fmt::OcWorldData& w) {
        // CARRIED, NOT UNDERSTOOD. The editor has no UI for a PCGVOLUME and doesn't need one, but
        // saveLevel builds a fresh OcWorldData from the editor's own state, so anything it doesn't
        // hold is GONE on the next save. That silently deleted every PCGVOLUME in the level, including
        // the sky field and the forest generator's own: open, save, and the record no longer exists.
        // The same reasoning as the project manifest keeping unknown keys: preserve what you don't understand.
        levelPcgVolumes_ = w.pcgVolumes;
        // ...and the rest of the header, for exactly the same reason. Placements and PCG volumes are
        // stripped because they are already held elsewhere; what is left is identity, BUILD, ALGO,
        // SPAWN, the environment numbers the editor does not expose, and the header comments
        // (w.notes: credits/licence lines), which saveLevel writes straight back out.
        levelHeader_ = w;
        levelHeader_.placements.clear();
        levelHeader_.pcgVolumes.clear();
        levelHeader_.lights.clear();   // rebuilt from the live CLight entities by saveLevel
        levelHeader_.decals.clear();   // ...and the DECAL records from the live CDecal entities
        levelHeader_.prefabInstances.clear();   // ...and the PREFABINST records from the live instances
        if (!levelPcgVolumes_.empty())
            AVER_INFO("[Level] carrying {} PCGVOLUME record(s) through the editor unchanged",
                      levelPcgVolumes_.size());

#if AVER_MODULE_FLUIDS
        // AND THE LEVEL'S OWN WATER, if it authored any: its WATER/WAVE records, before the
        // placements, as the runtime applies them.
        water_.applyLevel(*eng.device(), w);
#endif

        // AND NOW THE SKY FIELD ACTUALLY REACHES THE CLOUD LAYER, as it already did in the packaged
        // runtime: carrying the record through a save was all the editor ever did with it, so every
        // new project (scaffolded with `PCGVOLUME name Sky`) opened onto a bare gradient.
        // BY NAME, not "the first field": a level may declare a cave mask or moisture field too.
        // The floor is INVERTED into coverage: a HIGH density floor leaves LESS material standing, so
        // passing it through unchanged would clear the sky exactly when the author asked for overcast.
        for (const fmt::OcPcgVolume& v : levelPcgVolumes_) {
            if (v.name != "Sky") continue;
            const f64 floorV = v.coverageFloor < 0.0 ? 0.0 : (v.coverageFloor > 1.0 ? 1.0 : v.coverageFloor);
            sky_.cloudsEnabled = true;
            sky_.cloudSeed     = v.seed;
            sky_.cloudCoverage = static_cast<f32>(1.0 - floorV);
            AVER_INFO("[Level] sky field '{}' drives the cloud layer: seed {}, coverage {:.2f}",
                      v.name, sky_.cloudSeed, sky_.cloudCoverage);
            break;
        }

        // TERRAIN FIRST, THEN THE THINGS THAT STAND ON IT: this used to run at the end of loadLevel, harmless only while the ground was a flat plane -- a `snap` placement asks the ground how high it is.
#if AVER_MODULE_LANDSCAPE
        landscape_.setTerrainChangedHook([this] { applyLandscapeToStreaming(); });
        landscape_.setPathOverride(landscapeCliOverride_);
        pbr::MaterialSystem* landscapeMaterials = nullptr;
#if AVER_MODULE_PBR && AVER_MODULE_VOXI
        landscapeMaterials = &voxiRenderer_.materials();
#endif
        landscape_.loadForLevel(eng.device(), project_.contentDir(), levelPath, w, &content_, landscapeMaterials);
#else
        (void)eng; (void)levelPath;
#endif
    };
#if AVER_MODULE_LANDSCAPE
    // The same surface the scatter follows, so a hand-placed tree and a scattered fern sitting
    // a metre apart agree about where the ground is.
    hooks.groundHeightAt = [this](f64 x, f64 y, f64& outZ) { return landscape_.groundHeightAt(x, y, outZ); };
#endif
    // STAGED PROGRESS (GameLevel::LoadHooks::progress): forwarded into whichever LoadingScreen is
    // currently active -- openLevelDirect's own for a Content Browser open, applyProject's own when
    // this is the start level -- so both hosts share the identical parse/environment/placements
    // bands with no second rule. A load with no active screen (--play-test, a forwarded open with no
    // window) pays one null check per stage and nothing else.
    hooks.progress = [this](const std::string& stage, f32 fraction) {
        if (projectLoading_) projectLoading_->stage(stage.c_str(), fraction);
    };
    hooks.afterInstantiate = [this](const game::GameLevel::LoadedLevel& loaded) {
        if (loaded.legacy) onLegacyOcmapInstantiated(loaded);
        else               onLevelInstantiated(loaded);
#if AVER_MODULE_VOXI
        // FOLIAGE, AFTER PLACEMENTS -- the 75-90% band of the shared level-open contract, mirroring
        // Runtime/src/GameApp.cpp's installLevelHooks so the two hosts load it the same way. Runs for
        // a legacy .ocmap too (loaded.world is level_'s synthesised OcWorldData there, whose
        // foliageFiles is always empty -- loadLevelFoliage's own empty-list branch is then a no-op
        // that still clears whatever the previous level left). GATED ON voxiAttached_, same as
        // GameApp's identical call: setFoliage builds GPU-side tables and must not run before init()
        // has given the renderer a device.
        if (voxiAttached_) {
            if (projectLoading_) projectLoading_->stage("Loading foliage", 0.75f);
            const game::FoliageLoadResult fr = game::loadLevelFoliage(
                loaded.world, content_, &voxiRenderer_, project_.contentDir(),
                [this](f32 f) { if (projectLoading_) projectLoading_->progress(0.75f + f * 0.15f); });
            if (!fr.error.empty()) AVER_WARN("[Foliage] {}", fr.error);
        }
        if (projectLoading_) projectLoading_->stage("Finishing", 0.90f);
#endif
    };
    level_.setLoadHooks(std::move(hooks));
    level_.load(path, content_);
}

// What the editor keeps for an OCWORLD level once level_ has instantiated it: its label table, its
// entity->body map, and the per-entity record saveLevel needs.
void SandboxApp::onLevelInstantiated(const game::GameLevel::LoadedLevel& loaded) {
    const fmt::OcWorldData& w = loaded.world;
    const world::LevelInstance& inst = loaded.instance;
    levelEntities_ = inst.entities;
#if AVER_MODULE_PHYSICS
    levelBodies_ = inst.bodies;
#endif
    {
        // LIGHT records: one bare CLight entity each (the instantiator does not know them).
        scene::World& lw = scene::World::instance();
        for (const fmt::OcLight& r : w.lights) {
            const scene::Entity le = lw.create(r.name.empty() ? std::string("Light") : r.name, scene::kInvalidEntity,
                                               editor::transformFromRecord(r));
            if (le == scene::kInvalidEntity) continue;
            if (auto* c = static_cast<scene::CLight*>(lw.addComponent(le, scene::kComponentLight)))
                *c = editor::lightFromRecord(r);
            entityLabels_[static_cast<u32>(le)] = r.name.empty() ? std::string("Light") : r.name;
        }
        // PREFABINST records: each instance is rebuilt from its asset, with its overrides applied. The hooks add the
        // entities to levelEntities_ and label them.
        prefabModel_.setContentDir(project_.contentDir());
        prefabSys_.instantiateLevelInstances(w.prefabInstances);
        // DECAL records, the same way: one bare CDecal entity each.
        {
            const std::vector<scene::Entity> decals = editor::spawnDecalsFromRecords(lw, w.decals);
            for (usize k = 0; k < decals.size() && k < w.decals.size(); ++k)
                entityLabels_[static_cast<u32>(decals[k])] =
                    w.decals[k].name.empty() ? std::string("Decal") : w.decals[k].name;
        }
    }
    for (usize k = 0; k < inst.entities.size(); ++k) {
        const scene::Entity e = inst.entities[k];
        const fmt::OcWorldPlacement& p = w.placements[inst.placementIndex[k]];
        // makeEntityLabel RUNS REGARDLESS OF WHETHER p.name IS SET, and its result is thrown away
        // when it is: the ordinal it hands back also advances labelCounts_, and that advance has
        // to happen for every placement, named or not, or the counter falls out of step with what
        // saveLevel's own shadow copy of this same arithmetic assumes (see the name-vs-default
        // check in saveLevel, SandboxLevelEdit.cpp) -- a placement that carries an authored NAME
        // still occupies a slot in the "Wall 1, Wall 2, ..." sequence, it just isn't shown.
        const std::string generatedLabel = makeEntityLabel(p.material, p.asset);
        entityLabels_[static_cast<u32>(e)] = p.name.empty() ? generatedLabel : p.name;
        // WHY `collide` IS REMEMBERED AND THE MATERIAL IS NOT: the surface survives on the entity
        // (CMeshRenderer::material), but `nocollide` has no component at all -- a load-time
        // instruction nothing records afterwards. Inferring it from entityBodies_ would be wrong:
        // a level opened before aver_phys_init has no bodies for ANY placement.
        entityCollide_[static_cast<u32>(e)] = p.collide;
        // THE AUTHORED ANIMATION, remembered for the same reason: world::instantiate has already given
        // the entity a CAnimator, but that is the live clock Play advances, and the save must write what
        // the level said, not where a session left it.
        if (!p.animClip.empty())
            entityAnim_[static_cast<u32>(e)] = EntityAnim{p.animClip, p.animSpeed, p.animTime, p.animOnce};
        // AND `snap`, FOR THE SAME REASON AND A WORSE FAILURE. snapToGround has no component
        // either, but unlike nocollide it also changes what z MEANS: with it set, the authored z
        // is an offset ABOVE the terrain, and world::instantiate resolves it to ground + offset
        // before it ever reaches CLocal. So a save that rebuilt the placement from the live
        // transform wrote the resolved height back as if it were the offset AND dropped the
        // token -- baking the terrain into the file, on an open-and-save with no edits at all.
        // LevelClassSave.hpp already says exactly this and already handles it, for CLASS
        // placements only; this is the ordinary-placement half.
        if (p.snapToGround) entitySnapZ_[static_cast<u32>(e)] = p.z;
#if AVER_MODULE_PHYSICS
        if (inst.entityBody[k] >= 0) entityBodies_[static_cast<u32>(e)] = inst.entityBody[k];
#endif
    }
#if AVER_MODULE_SCENE
    // THE LEVEL'S SEQUENCE becomes the editor's: its tracks name placements, which are entities now.
    // levelHeader_ lets go of it, since saveLevel writes the sequence from the editor's model.
    {
        std::vector<scene::Entity> byPlacement(w.placements.size(), scene::kInvalidEntity);
        for (usize k = 0; k < inst.entities.size(); ++k) {
            const usize pi = static_cast<usize>(inst.placementIndex[k]);
            if (pi < byPlacement.size()) byPlacement[pi] = inst.entities[k];
        }
        seqEditor_.load(w.sequences, byPlacement);
        levelHeader_.sequences.clear();
    }
#endif
    // GRAPH-AS-CLASS / any other class placement: level_ collected them (level_.classPlacements());
    // spawnClassPlacements() spawns them later -- applyProject's "Loading level" stage runs BEFORE
    // "Starting scripts", so a class declared from a .ocgraph isn't registered yet here.

    // A level that states where the player starts gets a visible, movable marker for it. Without
    // this the SPAWN record was invisible in the editor: authored only by hand-editing the file,
    // and impossible to see or move once written.
    playerStart_ = scene::kInvalidEntity;
    if (w.hasSpawn) {
        playerStart_ = makePlayerStart(Vec3{static_cast<f32>(w.spawnX), static_cast<f32>(w.spawnY),
                                             static_cast<f32>(w.spawnZ)},
                                        static_cast<f32>(w.spawnYaw));
    }

    // FOG USED TO BE UNPACKED HERE, four lines above the call that now does it. Two places
    // reading the same record is how they drift, and this pair already had: this block set
    // fogDensity_ while saveLevel wrote a different member entirely.
    applyLevelSky(w);
    levelPath_ = loaded.path;
    levelName_ = w.name;
#if AVER_MODULE_SYNAPSE
    // The overlay is a DEVICE resource and this function has no device -- loadLevel is
    // reachable from paths that pass no Engine at all. Latched here and serviced from the
    // frame loop, which is the only place a line mesh can be created.
    navLoadPending_ = true;
#endif

    sel_ = -1;
    selEntity_ = scene::kInvalidEntity;

    // A STORED VIEWPOINT OUTRANKS AUTO-FRAMING, because it is the more specific statement:
    // frameCameraOnLevel guesses a view from the level's bounds, and a CAMERA record is where the
    // author actually was. Guessing is the fallback for a level that has never been saved with
    // one, which is every level written before the record existed.
    //
    // --cam STILL WINS, and it now does so BY ASKING rather than by ordering. The claim used to
    // be that the camOverride_ block in onInit runs after this one, which is true only for the
    // start map: --open-level is drained from the FRAME LOOP, long after onInit has applied the
    // override, so a level opened that way restored its own CAMERA record straight over it.
    // Measured: four captures at four different --cam positions returned the same probe
    // (72,71,79) and the same image, because none of the four cameras was ever used.
    //
    // A COMMAND LINE OUTRANKS STORED STATE. That is the same rule a project manifest already
    // learned the hard way when it silently outranked the render flags, and the failure has the
    // same shape both times: the run is not wrong, it just measures somewhere else, and nothing
    // says so. Refusing the restore here rather than re-applying the override afterwards keeps
    // one writer for the camera on this path instead of two that must stay in order.
    // BOTH WRITERS ARE SKIPPED, not just the restore: the frameCameraOnLevel fallback below is the
    // other way this function moves the camera, and letting it run would replace one silent
    // override with another.
    //
    // WHERE THE USER LEFT THE CAMERA OUTRANKS THE CAMERA RECORD: the record is only as new as the
    // level's last save, the remembered view is from the last time the level was left, saved or
    // not (storeLevelView).
    editor::LevelView leftView;
    if (camOverride_) {
        if (w.hasCamera) AVER_INFO("[Level] CAMERA record ignored -- --cam was given and outranks it");
    } else if (lookupLevelView(loaded.path, leftView)) {
        applyLevelView(leftView);
        levelCameraRestored_ = true;
        AVER_INFO("[Level] camera back where it was left: ({:.0f},{:.0f},{:.0f}) yaw {:.1f} pitch {:.1f} "
                  "(Saved/LevelViews.ini)", camPos_.x, camPos_.y, camPos_.z, leftView.yawDeg, leftView.pitchDeg);
    } else if (w.hasCamera) {
        applyLevelView(editor::LevelView{static_cast<f32>(w.camX), static_cast<f32>(w.camY),
                                         static_cast<f32>(w.camZ), static_cast<f32>(w.camYaw),
                                         static_cast<f32>(w.camPitch), static_cast<f32>(w.camSpeed)});
        levelCameraRestored_ = true;
        // LOGGED because this is otherwise invisible: a restored camera and an auto-framed one
        // look the same from outside the process, and the difference is exactly what a person
        // reporting "it didn't remember where I was" needs to be able to check.
        AVER_INFO("[Level] camera restored to ({:.0f},{:.0f},{:.0f}) yaw {:.1f} pitch {:.1f} speed {:.0f}",
                  camPos_.x, camPos_.y, camPos_.z, w.camYaw, w.camPitch, flySpeed_);
    } else if (!w.placements.empty()) {
        frameCameraOnLevel();
    }

#if AVER_MODULE_VOXI
    // OUTSIDE THE CAMERA CHAIN ABOVE, because fitting the GI volume used to happen at the bottom
    // of the camera framing -- which is only the fallback branch. A level with a saved CAMERA
    // record, or a run passing --cam, never fitted its volume at all: where the camera ends up and
    // how big the level is are different questions and only one of them is about the camera.
    //
    // BUT AN AUTHORED VOLUME STILL WINS, and that guard is not optional. RENDER.GIVOLUME is a
    // number a person chose; a fit computed from placement bounds is a guess, and a guess must
    // not silently replace authorship. I got this wrong first time round and measured the cost
    // on PTTest, which authors a 5659cm half-extent: fitting unconditionally REPLACED it with
    // 2356cm and shrank the volume for a level it already covered. project_.giExtent is 0 when
    // the manifest states nothing, which is exactly the "no author has an opinion" case the fit
    // is for.
    if (!w.placements.empty() && project_.giExtent <= 0.0f) fitGiVolumeToLevel();
#endif

    // JUST LOADED MEANS JUST SAVED, as far as the exit prompt is concerned. openLevelDirect
    // does NOT unload first, so without this a level opened after editing another would
    // inherit that one's history and be reported dirty the instant it appeared.
    markLevelSaved();
}

// What the editor keeps for a LEGACY .ocmap -- one that actually USES at least one of ROOT/CLIENT/
// SURFACE/GROUND/KILLZ/DEFORM -- once level_ has translated and instantiated it. A legacy file has
// none of the OCWORLD-only concerns above (header, water, sky, terrain, CAMERA), so it has its own
// callback rather than a branch through every section of that one.
void SandboxApp::onLegacyOcmapInstantiated(const game::GameLevel::LoadedLevel& loaded) {
    const fmt::OcMapData& m = *loaded.legacy;

    // CARRIED, NOT UNDERSTOOD -- see legacyMapHeader_'s own comment for which five record kinds
    // this is and why. saveLevelAsOcmap starts from this and overwrites only what the editor
    // genuinely owns, the identical levelHeader_ pattern the OCWORLD path above uses.
    legacyMapHeader_ = m;
    legacyMapHeader_.placements.clear();
    levelIsLegacyOcmap_ = true;

    // level_ TRANSLATED the placements into its shared pipeline (GameLevel.cpp's loadLegacyOcmap):
    // MATERIAL IS DELIBERATELY LEFT EMPTY on every synthesised placement, because a legacy PLACE
    // names a numeric SURFACE-table index and a DEFORM a soft-body material like "rubber", neither
    // an .ocmat name. Both values are preserved for the save through entityLegacySurface_/
    // entityLegacyMaterial_ instead, from the file's own records here.
    const world::LevelInstance& inst = loaded.instance;
    levelEntities_ = inst.entities;
#if AVER_MODULE_PHYSICS
    levelBodies_ = inst.bodies;
#endif
    for (usize k = 0; k < inst.entities.size(); ++k) {
        const scene::Entity e = inst.entities[k];
        const fmt::OcPlacement& p = m.placements[inst.placementIndex[k]];
        entityLabels_[static_cast<u32>(e)] = makeEntityLabel(std::string(), p.asset);
        entityCollide_[static_cast<u32>(e)] = true;
        // WHICH RECORD KIND THIS ENTITY CAME FROM, AND ITS OWN FIELD -- see the "MATERIAL IS
        // DELIBERATELY LEFT EMPTY" paragraph above for why these live here, and
        // entityLegacyDeform_'s own comment for what an entity absent from these maps saves as.
        entityLegacyDeform_[static_cast<u32>(e)] = p.deform;
        if (p.deform) entityLegacyMaterial_[static_cast<u32>(e)] = p.material;
        else          entityLegacySurface_[static_cast<u32>(e)] = p.surface;
#if AVER_MODULE_PHYSICS
        if (inst.entityBody[k] >= 0) entityBodies_[static_cast<u32>(e)] = inst.entityBody[k];
#endif
    }

    playerStart_ = scene::kInvalidEntity;
    if (m.hasSpawn) {
        playerStart_ = makePlayerStart(Vec3{static_cast<f32>(m.spawnX), static_cast<f32>(m.spawnY),
                                             static_cast<f32>(m.spawnZ)},
                                        static_cast<f32>(m.spawnYaw));
    }
    levelPath_ = loaded.path;
    levelName_ = m.name;

    sel_ = -1;
    selEntity_ = scene::kInvalidEntity;

    // Where the camera was left, else framed on the level (an .ocmap has no CAMERA record).
    editor::LevelView leftView;
    if (!camOverride_ && lookupLevelView(loaded.path, leftView)) {
        applyLevelView(leftView);
        levelCameraRestored_ = true;
    } else if (!loaded.world.placements.empty()) {
        frameCameraOnLevel();
    }
    // The legacy .ocmap path fitted the GI volume through the camera framing's old side effect too,
    // so it gets the explicit call for the same reason the OCWORLD path above does.
#if AVER_MODULE_VOXI
    if (!loaded.world.placements.empty() && project_.giExtent <= 0.0f) fitGiVolumeToLevel();
#endif
}

#endif

#if AVER_MODULE_SCENE
#if AVER_MODULE_FRAMEWORK
// GRAPH-AS-CLASS / any other class placement: mirrors GameLevel::spawnClassPlacements. Spawned
// FOR REAL (aver_fw_spawn), not previewed -- there's no "inert until Play" precedent to build on,
// and this matches how every ORDINARY mesh placement already behaves. Accepted consequence: a
// placed class runs OnTick immediately on load, even outside Play.
void SandboxApp::spawnClassPlacements() {
    const std::vector<fmt::OcWorldPlacement>& placements = level_.classPlacements();
    for (usize pi = 0; pi < placements.size(); ++pi) {
        const fmt::OcWorldPlacement& p = placements[pi];
        const int32_t c = aver_fw_class_find(p.className.c_str());
        if (c == 0) {
            AVER_WARN("[Level] placement names class '{}', which is not declared -- skipped", p.className);
            continue;
        }

        f64 pz = p.z;
#if AVER_MODULE_LANDSCAPE
        // Same ground query loadLevel hands level_ as hooks.groundHeightAt -- re-expressed here
        // because level_ keeps its hooks private and this is their only other caller.
        if (p.snapToGround) {
            f64 gz = 0.0;
            if (landscape_.groundHeightAt(p.x, p.y, gz)) pz = gz + p.z;
        }
#endif
        const f32 pos3[3]  = {static_cast<f32>(p.x), static_cast<f32>(p.y), static_cast<f32>(pz)};
        const Quat rot     = world::quatFromEulerDeg(Vec3{static_cast<f32>(p.roll),
                                                           static_cast<f32>(p.pitch),
                                                           static_cast<f32>(p.yaw)});
        const f32 quat4[4]  = {rot.x, rot.y, rot.z, rot.w};
        const f32 scale3[3] = {static_cast<f32>(p.sx), static_cast<f32>(p.sy), static_cast<f32>(p.sz)};

        const int32_t e = aver_fw_spawn(c, p.className.c_str(), pos3, quat4, scale3);
        if (e == 0) {
            AVER_WARN("[Level] class '{}' failed to spawn at ({:.0f}, {:.0f}, {:.0f})",
                      p.className, pos3[0], pos3[1], pos3[2]);
            continue;
        }
        levelClassInstances_.push_back(ClassInstance{pi, e});
    }
    if (!levelClassInstances_.empty())
        AVER_INFO("[Level] {} class instance(s) placed -- an entity exists for each; whether its graph "
                  "COMPILED is reported per instance above, because aver_fw_spawn returns a live entity "
                  "even when the managed bind behind it failed, so this count is placement, not success",
                  levelClassInstances_.size());
}

#endif
#endif

#if AVER_MODULE_SCENE
// Applies a level's SUN, SKY, FOG and CLOUDS records to the live atmosphere.
//
// The MAPPING is aver::assets::applyLevelEnv, shared with the shipped game -- see LevelSky.hpp
// for why it is one function and not two. What stays here is what is genuinely the EDITOR's:
// its own mirrors of the fields the Details sliders bind to, the record-present flags saveLevel
// reads, and the one warning that only makes sense to somebody authoring a level.
void SandboxApp::applyLevelSky(const fmt::OcWorldData& w) {
    assets::applyLevelEnv(w, sky_);

    // RESEEDED FROM THE ATMOSPHERE. The frame loop copies each of these back over sky_ every
    // frame (see the "FROZEN: sunDirection stays unnormalised" block), so applying a level and
    // not doing this would show its sky for exactly zero frames -- which is the shape the sun's
    // `lux` bug already had once: a level could state any intensity and every scene rendered at
    // the editor's default, silently, with a BIT-IDENTICAL frame either way.
    for (int i = 0; i < 3; ++i) {
        sunColor_[i]   = sky_.sunColor[i];
        skyZenith_[i]  = sky_.zenith[i];
        skyHorizon_[i] = sky_.horizon[i];
        fogColor_[i]   = sky_.fogColor[i];
    }
    sunAmbient_ = sky_.skyLightIntensity;
    fogDensity_ = sky_.fogDensity;

    if (w.hasSun) {
        // A sun below the horizon is legal -- the physical model renders night -- but under the
        // authored dome it silently lit everything from underneath, so no level was ever told.
        // Say it out loud rather than clamping: only the author knows if they meant it.
        f32 elev = 0.0f, azim = 0.0f;
        sky_.sunAngles(elev, azim);
        if (elev < 0.0f)
            AVER_WARN("[Level] SUN is {:.1f} degrees BELOW the horizon (dir {:.3f} {:.3f} {:.3f}). "
                      "The physical sky renders that as night; --sky-authored lights from below "
                      "as it always did.", elev, w.sunDir[0], w.sunDir[1], w.sunDir[2]);
        hasLevelSun_ = true;
    }
    if (w.hasSky)    hasLevelSky_    = true;
    if (w.hasFog)    hasLevelFog_    = true;
    if (w.hasClouds) hasLevelClouds_ = true;
}

#endif

#if AVER_MODULE_SCENE
#if AVER_MODULE_VOXI
// Fits the GI volume to the loaded level. Called on EVERY level load, whatever the camera does.
//
// THE BOUNDS ARE level_'s (GameLevel::placementBounds), the same numbers the shipped game fits its
// volume to. They were computed here by a copy of that loop, which had already drifted once.
//
// WHY THIS IS NOT PART OF THE CAMERA FRAMING. It lived at the bottom of the camera framing, which
// only runs for a level with NO saved CAMERA record -- the fallback for "we do not know where to
// look". So saving a level's camera silently stopped its GI volume being fitted, and the volume
// stayed at its authored default (centre 0,0,300, half-extent 1200cm) no matter how big the level
// was. Measured on Intel Sponza, which spans X -1386..747, Y -1595..1949, Z -43..1688: a third of
// the level sat outside its own GI volume, and nothing said so. Adding --cam made it worse for the
// same reason -- skipping the camera framing also skipped the fit.
//
// A radius, not a box, because that is what both consumers want: the camera wants a distance to
// stand back by, and setVolume takes a half-edge extent.
void SandboxApp::fitGiVolumeToLevel() {
    Vec3 lo{0, 0, 0}, hi{0, 0, 0};
    f32 radius = 1.0f;
    level_.placementBounds(lo, hi, radius);   // false (lo = hi = 0, radius 1) for a level with no placements
    const Vec3 centre{(lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f, (lo.z + hi.z) * 0.5f};
    giCenter_ = centre;
    giExtent_ = radius;
    AVER_INFO("[Voxi] GI volume fitted to the level: centre ({:.0f},{:.0f},{:.0f}) half-extent {:.0f}cm",
              centre.x, centre.y, centre.z, radius);
}

#endif
#endif

#if AVER_MODULE_SCENE
// Puts the editor camera where the whole loaded level is visible, and fits the fly speed to it.
void SandboxApp::frameCameraOnLevel() {
    Vec3 lo{0, 0, 0}, hi{0, 0, 0};
    f32 radius = 1.0f;
    level_.placementBounds(lo, hi, radius);
    const Vec3 centre{(lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f, (lo.z + hi.z) * 0.5f};
    const f32 dist = radius * 1.6f;
    camPos_ = Vec3{centre.x - dist * 0.65f, centre.y - dist * 0.65f, centre.z + dist * 0.55f};
    const Vec3 look = (centre - camPos_).getSafeNormal();
    yaw_   = std::atan2(look.y, look.x);
    pitch_ = std::asin(std::fmax(-1.0f, std::fmin(1.0f, look.z)));
    flySpeed_ = std::fmax(flySpeed_, radius * 0.02f);
    // This is a teleport, not a move: the next chunk-streaming update must not see this as a
    // (huge, one-frame) velocity computed against wherever the camera used to be.
    streaming_.resetVelocityTracking();
}

// Moves the editor camera to a stored view -- the level's CAMERA record or where it was last left.
void SandboxApp::applyLevelView(const editor::LevelView& v) {
    constexpr f32 kRad = 3.14159265358979323846f / 180.0f;
    camPos_ = Vec3{v.x, v.y, v.z};
    yaw_    = v.yawDeg * kRad;
    // Clamped to the same limits the mouse-look path enforces, so a hand-edited or corrupted file
    // cannot put the camera somewhere the controls can never recover from.
    pitch_  = std::fmax(-1.54f, std::fmin(1.54f, v.pitchDeg * kRad));
    // 0 means UNSTATED -- no stored speed leaves the user's preference alone rather than resetting
    // the camera to a stored zero and appearing to freeze.
    if (v.speed > 0.0f) flySpeed_ = std::fmax(20.0f, std::fmin(40000.0f, v.speed));
    streaming_.resetVelocityTracking();   // a teleport; see frameCameraOnLevel
}

// <project>/Saved/LevelViews.ini, or empty with no project: editor.ini deliberately holds nothing
// about a project, so a level outside one has nowhere to be remembered.
std::string SandboxApp::levelViewsPath() const {
    return project_.valid() ? project_.dir + "\\Saved\\LevelViews.ini" : std::string();
}

bool SandboxApp::lookupLevelView(const std::string& levelPath, editor::LevelView& out) const {
    if (maxFrames_ != 0) return false;
    const std::string file = levelViewsPath();
    const std::string key = editor::levelViewKey(levelPath, project_.dir);
    if (file.empty() || key.empty()) return false;
    std::string text;
    if (!readFileText(file, text)) return false;   // no file yet: nothing remembered
    const editor::LevelViewMap views = editor::parseLevelViews(text);
    const auto it = views.find(key);
    if (it == views.end()) return false;
    out = it->second;
    return true;
}

// Read-modify-write, so the other levels' entries (and a second editor's) survive. Skipped when the
// entry already says the same thing, so switching levels without moving never touches the file.
void SandboxApp::storeLevelView() {
    if (maxFrames_ != 0 || levelPath_.empty()) return;
    const std::string file = levelViewsPath();
    const std::string key = editor::levelViewKey(levelPath_, project_.dir);
    if (file.empty() || key.empty()) return;

    editor::LevelView v{camPos_.x, camPos_.y, camPos_.z, degrees(yaw_), degrees(pitch_), flySpeed_};
#if AVER_MODULE_FRAMEWORK
    // While a session drives the camera from the pawn, camPos_ is the pawn's eye, not the editor's.
    if (preplayViewValid_ && anyPlayActive() && !playEjected()) v = preplayView_;
#endif

    std::string text;
    readFileText(file, text);   // missing is fine: this is the first entry
    editor::LevelViewMap views = editor::parseLevelViews(text);
    if (const auto it = views.find(key); it != views.end()) {
        const editor::LevelView& o = it->second;
        if (o.x == v.x && o.y == v.y && o.z == v.z && o.yawDeg == v.yawDeg &&
            o.pitchDeg == v.pitchDeg && o.speed == v.speed)
            return;
    }
    views[key] = v;
    createDirectories(project_.dir + "\\Saved");
    // DirectWrite fallback, editor.ini's trade: a remembered camera is cache-grade, and a locked
    // file falling back to a plain write beats the view silently never reaching disk.
    AtomicWriteError err;
    if (!writeFileTextAtomic(file, editor::formatLevelViews(views), AtomicFallback::DirectWrite, &err)) {
        AVER_WARN("[Level] could not remember the camera for '{}' in {} ({})", key, file,
                  err.op.empty() ? "write failed" : err.op);
        return;
    }
    AVER_INFO("[Level] camera for '{}' remembered at ({:.0f},{:.0f},{:.0f})", key, v.x, v.y, v.z);
}

void SandboxApp::loadStartMap(Engine& eng) {
    // A MAP NAMED ON THE COMMAND LINE OUTRANKS THE PROJECT'S START MAP, loaded even when the
    // project has no start map at all -- the whole point of naming one. Checked before
    // project_.valid(), so a level outside any project still opens rather than silently opening nothing.
    if (!openMapPath_.empty()) {
        std::error_code ec;
        if (std::filesystem::exists(openMapPath_, ec)) { loadLevel(eng, openMapPath_); return; }
        AVER_ERROR("[Level] '{}' does not exist", openMapPath_);
        openMapPath_.clear();                  // fall back to the start map rather than nothing
    }
    if (!project_.valid() || project_.startMap.empty()) return;
    const std::string path = project_.contentDir() + "\\" + project_.startMap;
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        AVER_INFO("[Level] start map '{}' does not exist yet - the world starts empty", project_.startMap);
        levelName_ = std::filesystem::path(project_.startMap).stem().string();
        levelPath_ = path;
        return;
    }
    loadLevel(eng, path);
}

// Destroys the loaded level's entities and everything keyed to them: labels, bodies, undo.
void SandboxApp::unloadLevel(Engine& eng) {
    (void)eng;   // only read under AVER_MODULE_LANDSCAPE, at the end of this function
#if AVER_MODULE_FRAMEWORK
    // Every way out of a level (open, New, a project switch) ends a running session first, so object
    // animation stops and the Play snapshot is restored before the entities it names go away.
    if (anyPlayActive()) stopPlay();
#endif
    // FIRST, while levelPath_ still names the level being left: every way out of a level (another
    // level, New Level) comes through here. Closing the editor does not; onShutdown calls it too.
    storeLevelView();
#if AVER_MODULE_FRAMEWORK
    // BEFORE the raw-entity loop below, and through aver_fw_destroy rather than world.destroy() --
    // see GameLevel::unload's identical comment for why (the managed-dispatch unbind hook is what
    // releases a graph-class instance's GraphHost/VAR storage).
    for (const ClassInstance& ci : levelClassInstances_) aver_fw_destroy(ci.entity);
    levelClassInstances_.clear();
#endif
    scene::World& world = scene::World::instance();
    for (const scene::Entity e : levelEntities_) if (world.valid(e)) world.destroy(e);
    levelEntities_.clear();
#if AVER_MODULE_SCENE
    // LIGHT RECORDS are bare CLight entities that levelEntities_ does not hold (they are not placements).
    if (scene::ComponentPool* lights = world.pool(scene::kComponentLight)) {
        std::vector<scene::Entity> doomed;
        for (usize i = 0; i < lights->size(); ++i) doomed.push_back(lights->entityAt(i));
        for (const scene::Entity e : doomed) if (world.valid(e)) world.destroy(e);
    }
    // ...and DECAL RECORDS, by the CDecal pool, for the same reason (pooled gameplay decals die with it too).
    if (scene::ComponentPool* decals = world.pool(scene::kComponentDecal)) {
        std::vector<scene::Entity> doomed;
        for (usize i = 0; i < decals->size(); ++i) doomed.push_back(decals->entityAt(i));
        for (const scene::Entity e : doomed) if (world.valid(e)) world.destroy(e);
    }
#endif
#if AVER_MODULE_SCENE
    // THE PLAYERSTART MARKER needs its own line here for the reason it's not in levelEntities_:
    // deliberately transient, so saving doesn't emit it as a PLACE record. Consequence: loadLevel
    // reassigns playerStart_ on its way in, hiding this everywhere except File > New Level.
    // What that cost: the previous level's marker kept rendering in the new empty level, and
    // Toolbar > Add > Player Start SELECTED the stale entity instead of creating a new one.
    // Same shape, same place, same reasoning as the simulated fluid volume torn down further down:
    // anything a level owns but levelEntities_ doesn't hold must be released here by name.
    if (playerStart_ != scene::kInvalidEntity) {
        if (world.valid(playerStart_)) world.destroy(playerStart_);
        playerStart_ = scene::kInvalidEntity;
    }
    playerStartYaw_ = 0.0f;
#endif
    entityLabels_.clear();
    labelCounts_.clear();
    entityBodies_.clear();
    // Cleared with the rest of the level's state: carrying one level's PCG records into the
    // next would write them into a file that never had them.
    levelPcgVolumes_.clear();
    levelHeader_ = fmt::OcWorldData{};
#if AVER_MODULE_SCENE
    seqEditor_.reset();   // its actors are gone with the level; nothing to restore
#endif
    entityCollide_.clear();
    entityAnim_.clear();
    animEditBefore_.clear();
    entitySnapZ_.clear();
    // The legacy OCMAP state, cleared with the rest -- see levelIsLegacyOcmap_'s own comment
    // for why a stale `true` here would be worse than a stale levelHeader_: it would route the
    // NEXT level's save through the wrong writer entirely, not just lose a field it carries.
    levelIsLegacyOcmap_ = false;
    legacyMapHeader_ = fmt::OcMapData{};
    entityLegacyDeform_.clear();
    entityLegacySurface_.clear();
    entityLegacyMaterial_.clear();
    undoStack_.clear();
    redoStack_.clear();
    // A cleared history means nothing is pending against the file either.
    markLevelSaved();
    editToEntity_.clear();
    entityToEdit_.clear();
    editBeforeValid_ = false;
    if (sel_ == kSelScene) { sel_ = -1; selEntity_ = scene::kInvalidEntity; }
#if AVER_MODULE_PHYSICS
    for (const int32_t b : levelBodies_) aver_phys_remove_body(b);
    levelBodies_.clear();
#endif
#if AVER_MODULE_FLUIDS
    // THE ONLY teardown site for what a level spawned: this runs on every path that ends a level,
    // including File > New Level. The analytic surface is left standing.
    water_.unload();
#endif
    hasLevelFog_ = false;
    hasLevelSun_ = false;
    hasLevelSky_ = false;
    hasLevelClouds_ = false;
    // BACK TO THE EDITOR DEFAULT, now that the slider IS the saved value. While fog lived in two
    // members this happened for free: the level's copy went out of scope with levelFog_ and the
    // slider had never been touched. With one member, leaving it alone would carry one level's
    // weather into the next one that declares none.
    fogDensity_ = 4e-6f;
#if AVER_MODULE_VOXI
    // FOLIAGE LIVES OUTSIDE levelEntities_ ENTIRELY (no entity, no draw-list membership -- see
    // GameFoliage.hpp's own header comment), so nothing above this line touches it. Cleared here
    // rather than left for the next loadLevel's afterInstantiate to replace: New Level and a load
    // that fails after this point must not leave the PREVIOUS level's trees standing in an
    // otherwise-empty world. Gated on voxiAttached_ like every other post-init voxiRenderer_ call
    // this function's neighbours make (e.g. GameApp's identical hooks.afterUnload).
    if (voxiAttached_) voxiRenderer_.clearFoliage();
#endif
#if AVER_MODULE_PBR
    // LEVEL-SCOPED MATERIAL RESIDENCY (docs: PACKAGE level-materials): every way OUT of a level goes
    // through this function (this function's own opening comment), so it is the one place that can
    // release what the level being left needed without also having to know whether another one is
    // about to replace it. Releasing HERE, before the caller (loadLevel, startNewLevel) goes on to
    // load or not load a next level, is deliberately the "simplest correct order" rather than the
    // most efficient one: a name both the outgoing and incoming level share is released here and
    // reloaded a moment later by GameLevel::load()'s own materialForSurface() calls, which is a
    // redundant file read and texture upload for anything both levels use, traded for not needing
    // this function to know what "the next level" even is (unloadLevel() runs standalone from
    // startNewLevel() too, with no next level at all).
    //
    // pinnedMaterialNames_ is every name that must survive regardless -- see
    // pinMaterialResident()'s own comment for who is expected to populate it.
    if (levelScopedMaterialsEnabled()) {
        const usize released = content_.releaseMaterialsExcept(pinnedMaterialNames_);
        if (released)
            AVER_INFO("[Material] level-scoped residency: {} material(s) released on unload", released);
    }
#endif
    // AND level_'s OWN STATE: its environment, spawn record, PCG volumes, class placements and bounds.
    // Its entity and body lists are the load-time subset of the editor's own, already destroyed and
    // removed above; destroying an entity already destroyed and removing a removed body are no-ops.
    level_.unload();
    levelPath_.clear();
#if AVER_MODULE_LANDSCAPE
    landscape_.unload(eng.device());
    // The two editor-only sculpt flags landscape_.unload() itself does not know about.
    sculpting_ = false;
    sculptCursorValid_ = false;
#endif
}

#endif

} // namespace aver
