// GameLevel: loading a .ocworld/.ocmap into the scene, and taking it down again.
#pragma once
#include "aver/core/Types.hpp"
#include "aver/core/Math.hpp"
#include "aver/formats/OcProject.hpp"
#include "aver/pcg/PcgVolume.hpp"

#include <functional>
#include <string>
#include <vector>

#if AVER_MODULE_SCENE
#  include "aver/scene/World.hpp"
#  include "aver/formats/OcWorld.hpp"
#  include "aver/formats/OcMap.hpp"
#  include "aver/world/LevelInstance.hpp"
#  include "aver/game/LevelStreaming.hpp"
#  include "aver/formats/OcStream.hpp"
#endif
#if AVER_MODULE_SCENE && AVER_MODULE_PHYSICS
#  include "aver/formats/OcLanes.hpp"
#  include "aver/world/VehicleSystem.hpp"
#endif

namespace aver::game {

class GameContent;

// The currently loaded level.
//
// Everything the editor keeps ALONGSIDE the entities is deliberately absent: entity labels and
// their counts, the undo and redo stacks, the edit-to-entity maps, the selection. Those exist so a
// human can point at something and change it. A game has no selection, and carrying the structures
// anyway would mean carrying the code that maintains them.
//
// So is saveLevel. A shipped game does not write its levels back out, and a save path in a runtime
// that never calls it is a way to corrupt content nobody asked to modify.
class GameLevel {
public:
#if AVER_MODULE_SCENE
    // What one load() built, handed to LoadHooks::afterInstantiate. `world` is the parsed file -- for
    // a legacy .ocmap, its translation into OcWorldData -- and `instance` the entities and bodies
    // world::instantiate made from world.placements. `legacy` is the .ocmap's own records, or null.
    struct LoadedLevel {
        const std::string& path;
        const fmt::OcWorldData& world;
        const world::LevelInstance& instance;
        const fmt::OcMapData* legacy;
    };

    // What the host does around a level load, as hooks, so this class needs neither a device nor the
    // terrain and water classes. The editor does the same work inline in SandboxApp::loadLevel and
    // unloadLevel.
    struct LoadHooks {
        // After an OCWORLD file parses and before any placement is instantiated: where the editor
        // applies the level's water and loads its terrain, so a `snap` placement finds the ground.
        // Not called for a legacy .ocmap, whose editor load does neither.
        std::function<void(const std::string& path, const fmt::OcWorldData& w)> beforePlacements;
        // The ground height at a world XY, for `snap` mesh placements and class placements alike.
        std::function<bool(f64 worldXCm, f64 worldYCm, f64& outGroundZCm)> groundHeightAt;
        // At the end of unload(), for whatever beforePlacements created.
        std::function<void()> afterUnload;
        // STAGED LOADING PROGRESS: called with a stage name and a fraction in [0, 1] covering load()'s
        // OWN three steps -- parsing the file, beforePlacements (environment/terrain), and instantiating
        // placements (this last one driven by world::InstantiateOptions::progress, forwarded straight
        // through) -- 0 at the very start, 1 once instantiate() has placed everything. load() knows
        // nothing about what either host does in afterInstantiate (foliage, spawning class placements,
        // its own "finishing" tail), so the fraction never claims to cover that; the host rescales this
        // into whatever band its own loading screen reserves for "loading the level" (see
        // sandbox/src/SandboxLevelLoad.cpp / Runtime/src/GameApp.cpp for the two hosts' own bands).
        // Optional, and cheap to leave unset: a host with no loading screen pays nothing extra.
        std::function<void(const std::string& stage, f32 fraction)> progress;
        // At the end of a load that succeeded, with everything it built -- for a host that keeps
        // its own record per placed entity (the editor's labels, undo and save bookkeeping).
        // Called for a legacy .ocmap too, whose original records come in `legacy`.
        std::function<void(const LoadedLevel& loaded)> afterInstantiate;
    };
    void setLoadHooks(LoadHooks hooks) { hooks_ = std::move(hooks); }

    // Loads a .ocworld/.ocmap. Unloads whatever was loaded first. Dispatches on
    // fmt::levelFileIsLegacyOcmap, as the editor's loadLevel does: a legacy .ocmap is translated into
    // OcWorldData placements and spawn, then goes through the same pipeline.
    void load(const std::string& path, GameContent& content);

    // Loads the project's start map, unless `overridePath` names a level to open instead -- the
    // editor's loadStartMap/openMapPath_ precedence: the override is checked first, even with no valid
    // project, used exactly as given, and a file that does not exist falls back to the start map.
    //
    // A missing start map is INFO, not an error: a fresh project has a manifest naming a map nobody
    // has authored yet, and refusing to start would be wrong.
    void loadStartMap(const fmt::ProjectDesc& project, GameContent& content,
                       const std::string& overridePath = std::string());

    // Destroys the level's entities and its physics bodies.
    void unload();

#if AVER_MODULE_FRAMEWORK
    // GRAPH-AS-CLASS / any other class placement: spawns (aver_fw_spawn) one real actor per
    // placement load() found with a non-empty className, at its placement transform. SEPARATE from
    // load() ITSELF, deliberately -- see this class's own classPlacements_ comment for the ordering
    // reason. A no-op when load() found no class placements, or when scripting never became ready
    // (aver_fw_class_find simply finds nothing and each is skipped with a warning -- the same
    // "content that cannot resolve degrades to a warning, not a crash" contract every other
    // placement lookup in this file already has).
    void spawnClassPlacements();
#endif

    usize entityCount() const { return levelEntities_.size() + streaming_.residentEntities(); }
    // The animated placements load() made kinematic bodies for: what the host hands to
    // world::driveKinematicBodies each frame after the animation tick. Empty without physics.
    const std::vector<world::AnimatedBody>& animatedBodies() const {
        return streaming_.active() ? streaming_.animatedBodies() : animatedBodies_;
    }

    // LEVEL STREAMING (docs/LEVEL_STREAMING.md): a level with a STREAM record is streamed by
    // streaming() instead of instantiated whole, when a device was given before load().
    void setDevice(rhi::IDevice* device) { device_ = device; }
    void setStreamHooks(LevelStreaming::Hooks hooks) { streamHooks_ = std::move(hooks); }
    LevelStreaming& streaming() { return streaming_; }
    const LevelStreaming& streaming() const { return streaming_; }
    // Once a frame, before World::flush: streams around `viewers` and retires released meshes.
    void tickStreaming(const std::vector<Vec3>& viewers);
    // The loaded streaming data's foliage tables, and whether it no longer matches the level.
    const std::vector<fmt::OcStreamFoliage>& streamFoliage() const { return streamFoliage_; }
    bool streamDataStale() const { return streamDataStale_; }
    // Absolute cell-table paths parallel to `foliageFiles` (empty entry = the Content original).
    std::vector<std::string> foliageTablePaths(const std::vector<std::string>& foliageFiles) const;
    // THE LEVEL'S VEHICLE PLACEMENTS: every placement that carried a `vehicle <preset>` token, by entity,
    // WHETHER OR NOT it can become a car right now -- the record is what a save writes the token back
    // from, and a placement whose mesh has no bounds today (or a build with no physics module at all)
    // must not lose its token for it. Collected by load(), emptied by unload(); nothing here is
    // simulated until a host calls beginVehicles. Like a CRigidBody, a car is data at load and becomes
    // real when play starts, so a level that is only being looked at never has one settling on its
    // suspension.
    //
    // The preset `e` was placed with, or null for an entity that is not a vehicle placement. The editor
    // keeps this current for what it makes after the load (setVehiclePreset), so a copy it pastes is an
    // ordinary mesh but a delete that is undone is still a car.
    const std::string* vehiclePresetOf(scene::Entity e) const;
    // Records that `e` is a vehicle placement of `preset`, replacing an earlier record for it; an empty
    // preset forgets it. load() fills the list from the file itself: this is for the editor, which
    // creates or recreates entities after the load (an undone delete, a placement added over MCP) and
    // must not lose the token when it saves.
    void setVehiclePreset(scene::Entity e, const std::string& preset);
#if AVER_MODULE_PHYSICS
    // The lane graph from the level's <name>.oclanes sidecar, or null when there is none (or no
    // vehicle placement to use one). A level with cars and no lanes is legal: they park.
    const fmt::OcLanesData* lanes() const { return hasLanes_ ? &lanes_ : nullptr; }
    // Where a level's lane sidecar lives: <level>.oclanes beside the file, under its stem. A recovered
    // autosave (`<level>.autosave`, which the editor opens by that path) names its LEVEL'S lanes, not a
    // file of its own. Public so the editor's Save As can copy the sidecar along with the level.
    static std::string laneSidecarPath(const std::string& levelPath);
    // PLAY START, for both hosts: builds one physics car per vehicle placement that can become one, at
    // its entity's CURRENT world transform, on this level's lanes, with a seed derived from the level's
    // name so the same level drives the same way every run. A placement is skipped (and counted in one
    // log line) when its entity is gone (the editor may have deleted it since the load), when its mesh
    // has no known bounds -- read from the entity's mesh NOW, so a Change Mesh is honoured -- when it is
    // not at scale 1, or when it also carries an object animation, whose clip would overwrite the
    // physics pose every frame. Returns how many cars exist afterwards (0 when there are none, or
    // physics is not up). The host ends them with vehicles.end() and seeds vehicles.entities() into
    // PlayMobility.
    usize beginVehicles(world::VehicleSystem& vehicles, const GameContent& content) const;
#endif
    const std::string& name() const { return levelName_; }
    const std::string& path() const { return levelPath_; }
    bool hasFog() const { return env_.hasFog; }

    // A density field the level declared, resolved to something samplable. `bounded` carries the
    // spec a VolumeBuilder can fill; `infinite` is sampled directly by world position.
    struct PcgField {
        std::string name;
        bool infinite = true;
        pcg::InfiniteSpec infiniteSpec{};
        pcg::VolumeSpec   boundedSpec{};
        Vec3 boundsMin{}, boundsMax{};
    };
    const std::vector<PcgField>& pcgFields() const { return pcgFields_; }
    // The same records unresolved, for chunk streaming (GameStreaming::enable), which reads fields
    // pcgFields() drops -- the editor keeps levelPcgVolumes_ and its SCATTER list for the same reason.
    const std::vector<fmt::OcPcgVolume>& pcgVolumes() const { return pcgVolumes_; }
    const std::vector<fmt::OcScatterSpecies>& scatterSpecies() const { return scatterSpecies_; }
#if AVER_MODULE_FRAMEWORK
    // The placements load() found with a class name, in file order, for spawnClassPlacements or a
    // host that spawns them itself.
    const std::vector<fmt::OcWorldPlacement>& classPlacements() const { return classPlacements_; }
#endif

    // The field of that name, or null. By NAME because a level may declare several and picking
    // "the first one" is how the wrong field gets sampled without anything saying so.
    const PcgField* pcgField(const std::string& name) const;
    // THE WHOLE ENVIRONMENT, IN ONE OBJECT, and that is the point rather than a tidying.
    //
    // This class used to mirror twelve of the format's fields into members of its own, one
    // assignment each in load(). The consequence was that adding a field to .ocworld and to the
    // editor left it AUTHORABLE AND SILENTLY IGNORED at runtime until somebody remembered a sixth
    // place, and there was nothing to make them remember. fmt::OcWorldEnv is the base of
    // OcWorldData precisely so `env_ = w` can slice off exactly these fields, so there is now no
    // sixth place to forget.
    const fmt::OcWorldEnv& env() const { return env_; }
    const fmt::OcStreamSettings& streamSettings() const { return stream_; }

    // The level's placement bounds, and the bounding-sphere radius of them. False when the level has
    // no placements at all, which is the case a caller must not turn into a zero-sized GI volume.
    bool placementBounds(Vec3& lo, Vec3& hi, f32& radius) const;

    bool hasSky() const { return env_.hasSky; }

    // The level's authored Player Start / SPAWN record, captured once by load(). Mirrors
    // SandboxViewport.cpp's playerStartTransform() -- specifically its SECOND branch, the raw SPAWN
    // record -- minus the FIRST branch, which prefers a live PlayerStart marker entity: a shipped
    // game has no such marker (see this class's own header comment on what a game does not carry
    // over from the editor), so the SPAWN record IS the whole answer here. `valid` is false when the
    // level declares none at all, the same "caller decides the fallback" contract placementBounds()
    // above already uses -- GameApp::placePawnAtSpawn() leaves the pawn wherever the GameMode put it.
    struct SpawnPoint {
        Vec3 position{};
        f32 yawDeg = 0.0f;
        bool valid = false;
    };
    const SpawnPoint& spawn() const { return spawn_; }
#endif

private:
#if AVER_MODULE_SCENE
    LevelStreaming streaming_;
    LevelStreaming::Hooks streamHooks_;
    rhi::IDevice* device_ = nullptr;
    GameContent* content_ = nullptr;
    u64 streamFrame_ = 0;
    std::vector<u64> heldMeshes_;   // a whole level's meshes, acquired at load
    std::vector<fmt::OcStreamFoliage> streamFoliage_;
    bool streamDataStale_ = false;
    std::vector<scene::Entity> levelEntities_;
    // See animatedBodies(). Cleared by unload() before the bodies go.
    std::vector<world::AnimatedBody> animatedBodies_;
    std::string levelPath_;
    std::string levelName_;
    // The sun, sky, fog and clouds this level declared, copied whole. See env() above.
    fmt::OcWorldEnv env_{};
    fmt::OcStreamSettings stream_{};
    std::vector<PcgField> pcgFields_;
    std::vector<fmt::OcPcgVolume> pcgVolumes_;
    std::vector<fmt::OcScatterSpecies> scatterSpecies_;

    // Placement bounds, accumulated by load(). hasBounds_ stays false for a level with no
    // placements, which is a real case (an empty start map) and not a zero-sized world.
    bool hasBounds_ = false;
    Vec3 boundsLo_{}, boundsHi_{};

    // The level's SPAWN record, captured by load(). See spawn()'s own comment above.
    SpawnPoint spawn_{};

    LoadHooks hooks_;

    // See vehiclePresetOf(). In FILE ORDER, which seeds each car's random stream, so the same level drives
    // the same way every run.
    struct VehiclePlacement {
        scene::Entity entity = scene::kInvalidEntity;
        std::string preset;
    };
    std::vector<VehiclePlacement> vehiclePlacements_;

#  if AVER_MODULE_PHYSICS
    std::vector<int32_t> levelBodies_;
    // See lanes().
    fmt::OcLanesData lanes_;
    bool hasLanes_ = false;
#  endif

    // GRAPH-AS-CLASS / any other class placement: entities spawned (aver_fw_spawn) by
    // spawnClassPlacements() -- DISJOINT from levelEntities_, which only ever holds RAW
    // mesh-placement entities (see LevelInstance.cpp's own "skip, framework-free" comment for why a
    // class placement never becomes one of those). Torn down through aver_fw_destroy, not
    // world.destroy(), so a graph-class instance's managed-dispatch unbind hook actually fires (see
    // HostBridge.cs's DispUnbind) and its GraphHost/VAR storage is released rather than merely
    // orphaned.
#  if AVER_MODULE_FRAMEWORK
    std::vector<int32_t> levelClassInstances_;

    // The placements load() found with a non-empty className, held here rather than spawned
    // immediately FROM load() -- ORDERING, not style: BOTH composition roots load their start level
    // BEFORE the scripting host declares graph classes (GameApp::openProject runs before
    // GameApp::initScripting/declareGraphClasses in onInit; SandboxApp::applyProject's own "Loading
    // level" stage runs before its "Starting scripts" stage, identically). Calling aver_fw_class_find
    // from inside load() itself would therefore ALWAYS miss a graph class, silently, on every single
    // project -- this is exactly the "seam wired in the wrong order looks like it works until you
    // check" trap aee2404 already exists to name. Collecting here and spawning from a SEPARATE,
    // explicitly-later call (spawnClassPlacements(), called once scripting is ready) is what keeps
    // "load the level" and "spawn its class instances" as two ordered steps rather than one that
    // silently assumes an ordering neither root actually has.
    std::vector<fmt::OcWorldPlacement> classPlacements_;
#  endif
#endif
};

} // namespace aver::game
