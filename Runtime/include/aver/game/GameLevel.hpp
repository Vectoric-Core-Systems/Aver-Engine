// GameLevel: loading a .ocworld/.ocmap into the scene, and taking it down again.
#pragma once
#include "aver/core/Types.hpp"
#include "aver/core/Math.hpp"
#include "aver/formats/OcProject.hpp"
#include "aver/pcg/PcgVolume.hpp"

#include <string>
#include <vector>

#if AVER_MODULE_SCENE
#  include "aver/scene/World.hpp"
#  include "aver/formats/OcWorld.hpp"
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
    // Loads a .ocworld/.ocmap. Unloads whatever was loaded first.
    void load(const std::string& path, GameContent& content);

    // Loads the project's start map. A missing start map is INFO, not an error: a fresh project has
    // a manifest naming a map nobody has authored yet, and refusing to start would be wrong.
    void loadStartMap(const fmt::ProjectDesc& project, GameContent& content);

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

    usize entityCount() const { return levelEntities_.size(); }
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
    std::vector<scene::Entity> levelEntities_;
    std::string levelPath_;
    std::string levelName_;
    // The sun, sky, fog and clouds this level declared, copied whole. See env() above.
    fmt::OcWorldEnv env_{};
    std::vector<PcgField> pcgFields_;

    // Placement bounds, accumulated by load(). hasBounds_ stays false for a level with no
    // placements, which is a real case (an empty start map) and not a zero-sized world.
    bool hasBounds_ = false;
    Vec3 boundsLo_{}, boundsHi_{};

    // The level's SPAWN record, captured by load(). See spawn()'s own comment above.
    SpawnPoint spawn_{};

#  if AVER_MODULE_PHYSICS
    std::vector<int32_t> levelBodies_;
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
