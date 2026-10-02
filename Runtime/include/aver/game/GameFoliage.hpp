// GameFoliage: loading a level's instanced foliage into the renderer, for both hosts.
//
// A level's instanced foliage lives OUTSIDE the entity/draw system entirely -- see
// modules/render.voxi/include/aver/voxi/VoxiRenderer.hpp's FoliagePrototype/FoliageInstance/
// setFoliage for the shape it lands in: static, ray-traced only (TLAS), never in
// VoxiRenderer::draws_, no collision, not individually selectable. It is a LOAD, once per level
// open, not a placement loop -- the reason this is one function rather than a scene::World walk.
//
// SHARED BY THE EDITOR AND THE RUNTIME, the same way GameLevel/GameRender already are: both call
// this from their own GameLevel::LoadHooks::afterInstantiate, after placements are instantiated,
// with their own GameContent and voxi::VoxiRenderer.
#pragma once
#include "aver/core/Types.hpp"

#include <functional>
#include <string>

#if AVER_MODULE_SCENE && AVER_MODULE_VOXI
#include "aver/formats/OcWorld.hpp"

namespace aver::voxi { class VoxiRenderer; }

namespace aver::game {

class GameContent;

// What one call to loadLevelFoliage did, for the caller's own summary log line -- mirrors
// world::LevelInstance's own "counted here because only this loop knows" reasoning.
struct FoliageLoadResult {
    u32 files = 0;          // FOLIAGE records that resolved to a path and parsed
    u32 groups = 0;         // OcInstanceGroup entries carried into a FoliagePrototype, across every file
    u32 instances = 0;      // total instances handed to voxi::VoxiRenderer::setFoliage
    u32 droppedParts = 0;   // translucent parts dropped (Voxi never receives a blended instance)
    // Non-empty when at least one FOLIAGE file could not be read or parsed at all (the reason
    // named is the LAST such failure; each is also logged individually as it happens). A missing
    // mesh or an empty level are warnings only, and do not set this -- they are not why the level
    // failed to load its foliage, they are content gaps this function already reports on its own.
    std::string error;
};

// Loads every FOLIAGE file `w.foliageFiles` names into `voxi` (voxi::VoxiRenderer::setFoliage), or
// clears whatever was there when the level names none. Each group's mesh and material are resolved
// EXACTLY the way game::drawWorld's resolveDrawLook does (Runtime/src/GameRender.cpp, the
// authored > dead-handle > named-look > flat-fallback ladder), through the same shared
// aver::game::resolveSurfaceLook (SceneSubmission.hpp) that lambda calls -- see this function's own
// definition for the one place it has to replicate resolveDrawLook's TWO GameContent lookups by
// hand rather than call it directly (it is a closure private to GameRender.cpp, not a free function).
//
// `contentDir` is the project's content root, against which each FOLIAGE record's content-relative
// path resolves -- the same convention GameContent::resolveMaterialGraph's GRAPHREF already uses.
// `progress`, when given, is called with a fraction in [0, 1] as files finish loading; the caller
// rescales it into whatever band its own loading screen reserves for foliage (see
// sandbox/src/SandboxLevelLoad.cpp / Runtime/src/GameApp.cpp for the two hosts' own bands).
//
// A NULL `voxi` is a legal no-op (every field 0, nothing read or written): a host with no attached
// Voxi renderer -- AVER_MODULE_VOXI compiled in but the feature never attached -- has nowhere to
// hand instances to, and asking it to would be reaching through a null pointer for nothing.
FoliageLoadResult loadLevelFoliage(const fmt::OcWorldData& w, GameContent& content,
                                    voxi::VoxiRenderer* voxi, const std::string& contentDir,
                                    const std::function<void(f32 fraction)>& progress = {});

} // namespace aver::game

#endif // AVER_MODULE_SCENE && AVER_MODULE_VOXI
