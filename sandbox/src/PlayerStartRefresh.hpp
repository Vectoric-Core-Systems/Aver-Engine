#pragma once
// SandboxApp's Player Start cached-handle refresh, pulled out as a free function over plain
// scene::World + std::vector<Entity>, so a headless test can exercise it against a REAL scene::World
// -- no App, no Engine, no window. See SandboxApp::refreshPlayerStart's own comment for the full story
// of why a cached handle needs re-deriving at all (undo/redo/delete can each invalidate it out from
// under the cache) and for the pointer-comparison bug this used to hide: both comparisons here used
// to read `w.name(x) == "PlayerStart"`, which compares a `const char*` against a string literal with
// `==` -- POINTERS, not characters -- so neither branch could ever match, the early-out never fired,
// and playerStart_ was silently wiped to kInvalidEntity on every undo, redo and delete.
//
// WHOLE-FILE GUARDED, matching EditorEntitySnapshot.hpp's own precedent: scene::World and
// scene::Entity exist only when Aver.Scene is linked (AVER_MODULE_SCENE=1), and a tree configured
// with that module off has no Aver.Scene include directory at all -- SandboxApp.cpp's own
// refreshPlayerStart() is itself compiled out under the same guard.
#if AVER_MODULE_SCENE
#include "aver/scene/World.hpp"

#include <string_view>
#include <vector>

namespace aver::editor {

// `current`: the cached handle, possibly stale or kInvalidEntity. `levelEntities`: the level's own
// entity list (SandboxApp's levelEntities_) -- the walk below is deliberately scoped to THIS, not to
// every live entity in the one process-global World, matching the level-ownership boundary
// isLevelOwned() is itself keyed on. world::find(std::string_view) was considered and rejected for
// exactly that reason: its linear scan covers the WHOLE World, so a same-named entity that is not
// part of this level (a stray leftover, or one belonging to something else entirely) would make it
// return the wrong handle with nothing to signal the mismatch. Returns `current` unchanged if it is
// still a live entity named "PlayerStart"; else the first such entity found while walking
// `levelEntities` in order; else kInvalidEntity.
inline scene::Entity refreshPlayerStart(const scene::World& w, scene::Entity current,
                                         const std::vector<scene::Entity>& levelEntities) {
    if (current != scene::kInvalidEntity && w.valid(current) &&
        std::string_view(w.name(current)) == "PlayerStart")
        return current;
    for (const scene::Entity e : levelEntities)
        if (w.valid(e) && std::string_view(w.name(e)) == "PlayerStart") return e;
    return scene::kInvalidEntity;
}

} // namespace aver::editor
#endif // AVER_MODULE_SCENE
