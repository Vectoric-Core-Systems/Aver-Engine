// GameLevel: loading a .ocworld/.ocmap into the scene, and taking it down again.
#pragma once
#include "aver/core/Types.hpp"
#include "aver/core/Math.hpp"
#include "aver/formats/OcProject.hpp"

#include <string>
#include <vector>

#if AVER_MODULE_SCENE
#  include "aver/scene/World.hpp"
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

    usize entityCount() const { return levelEntities_.size(); }
    const std::string& name() const { return levelName_; }
    const std::string& path() const { return levelPath_; }
    bool hasFog() const { return hasLevelFog_; }
    f32 fogDensity() const { return levelFog_; }
    const f32* fogColor() const { return fogColor_; }
#endif

private:
#if AVER_MODULE_SCENE
    std::vector<scene::Entity> levelEntities_;
    std::string levelPath_;
    std::string levelName_;
    bool hasLevelFog_ = false;
    f32  levelFog_ = 0.0002f;
    f32  fogColor_[3] = {0.55f, 0.60f, 0.68f};
#  if AVER_MODULE_PHYSICS
    std::vector<int32_t> levelBodies_;
#  endif
#endif
};

} // namespace aver::game
