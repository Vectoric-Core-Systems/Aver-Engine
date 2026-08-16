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
    bool hasFog() const { return hasLevelFog_; }

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
    f32 fogDensity() const { return levelFog_; }
    const f32* fogColor() const { return fogColor_; }

    // Level's sun and sky settings
    bool hasSun() const { return hasSun_; }
    const f64* sunDir() const { return sunDir_; }
    const f64* sunColor() const { return sunColor_; }
    f64 sunLux() const { return sunLux_; }

    // The level's placement bounds, and the bounding-sphere radius of them. False when the level has
    // no placements at all, which is the case a caller must not turn into a zero-sized GI volume.
    bool placementBounds(Vec3& lo, Vec3& hi, f32& radius) const;

    bool hasSky() const { return hasSky_; }
    bool skyPhysical() const { return skyPhysical_; }
    f64 skyMieScatter() const { return skyMieScatter_; }
    f64 skyMultiScatter() const { return skyMultiScatter_; }
    i32 skyViewSteps() const { return skyViewSteps_; }
    i32 skyAerialSteps() const { return skyAerialSteps_; }
#endif

private:
#if AVER_MODULE_SCENE
    std::vector<scene::Entity> levelEntities_;
    std::string levelPath_;
    std::string levelName_;
    bool hasLevelFog_ = false;
    f32  levelFog_ = 0.0002f;
    f32  fogColor_[3] = {0.55f, 0.60f, 0.68f};
    std::vector<PcgField> pcgFields_;

    // Sun and sky settings from the level
    bool hasSun_ = false;
    f64 sunDir_[3] = {-0.5481, 0.3838, 0.7431};
    f64 sunColor_[3] = {1.0, 0.98, 0.92};
    f64 sunLux_ = 100000.0;

    // Placement bounds, accumulated by load(). hasBounds_ stays false for a level with no
    // placements, which is a real case (an empty start map) and not a zero-sized world.
    bool hasBounds_ = false;
    Vec3 boundsLo_{}, boundsHi_{};

    bool hasSky_ = false;
    bool skyPhysical_ = true;
    f64 skyMieScatter_ = -1.0;
    f64 skyMultiScatter_ = -1.0;
    i32 skyViewSteps_ = 0;
    i32 skyAerialSteps_ = 0;

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
