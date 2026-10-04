// editor::refreshPlayerStart (PlayerStartRefresh.hpp): SandboxApp's Player Start cached-handle
// refresh, extracted so this can run against a REAL scene::World with no App, no Engine and no
// window. See the header's own comment for the bug this pins down: both comparisons inside it used
// to read `w.name(x) == "PlayerStart"` -- a `const char*` compared against a string literal with
// `==`, which compares POINTERS, not characters, so neither branch could ever match. That silently
// wiped playerStart_ to kInvalidEntity on every undo, redo and delete -- precisely the failure
// SandboxApp::refreshPlayerStart's own comment says the function exists to prevent.
//
// NON-VACUOUS, CHECKED BY HAND: reverting PlayerStartRefresh.hpp's two `std::string_view(...) ==`
// comparisons back to the original `w.name(...) == "PlayerStart"` and rebuilding this target makes
// "finds it by walking levelEntities" and "recovers after the cached handle goes stale" below FAIL
// (both compare a live pointer against a string literal, which are never equal) -- confirming this
// suite would have caught the regression, not merely exercised the fixed code.
#include "PlayerStartRefresh.hpp"

#include "aver/core/Log.hpp"
#include "aver/scene/World.hpp"

#include <string>
#include <vector>

using namespace aver;
using namespace aver::scene;

static int g_checks = 0, g_failures = 0;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    AVER_ERROR("  FAIL  {}", what);
    ++g_failures;
}

int main() {
    AVER_INFO("PlayerStartRefreshTest");
    World& w = World::instance();

    AVER_INFO("an empty level has nothing to find");
    {
        const std::vector<Entity> levelEntities;
        const Entity result = editor::refreshPlayerStart(w, kInvalidEntity, levelEntities);
        check(result == kInvalidEntity, "no cached handle, empty level -> kInvalidEntity");
    }

    AVER_INFO("finds it by walking levelEntities when the cached handle is invalid");
    {
        const Entity decoy = w.create("Meshes/cube.ocmesh");
        const Entity marker = w.create("PlayerStart");
        const std::vector<Entity> levelEntities = {decoy, marker};

        const Entity result = editor::refreshPlayerStart(w, kInvalidEntity, levelEntities);
        check(result == marker, "walked the list and matched by NAME, not position (marker is second)");

        w.destroy(decoy);
        w.destroy(marker);
    }

    AVER_INFO("an already-good cached handle is returned unchanged, with no rescan needed");
    {
        const Entity marker = w.create("PlayerStart");
        const std::vector<Entity> levelEntities;   // deliberately NOT containing marker
        const Entity result = editor::refreshPlayerStart(w, marker, levelEntities);
        check(result == marker, "the early-out path recognises its own cached handle");
        w.destroy(marker);
    }

    AVER_INFO("recovers after the cached handle goes stale (destroy, or an undo/redo that recreated it under a new handle)");
    {
        const Entity oldMarker = w.create("PlayerStart");
        w.destroy(oldMarker);   // only QUEUES the destroy -- see World::destroy's own comment
        w.flush();              // retires it for real, so w.valid(oldMarker) is now false
        const Entity newMarker = w.create("PlayerStart");   // recreateFrom mints a NEW handle
        const std::vector<Entity> levelEntities = {newMarker};

        const Entity result = editor::refreshPlayerStart(w, oldMarker, levelEntities);
        check(result == newMarker, "stale cached handle is dropped and the live one is re-found");
        w.destroy(newMarker);
    }

    AVER_INFO("SCOPE: an entity named PlayerStart that is NOT in levelEntities is not returned -- "
              "the walk is deliberately scoped to the level's own list, not the whole World");
    {
        // The entity this test's own earlier cases already destroyed prove the World can hold OTHER
        // live entities meanwhile; this one is the actual regression world::find(std::string_view)
        // would have introduced, since that scans every live entity in the process-global World.
        const Entity foreign = w.create("PlayerStart");   // deliberately NOT pushed into levelEntities
        const std::vector<Entity> levelEntities;          // this "level" owns nothing
        const Entity result = editor::refreshPlayerStart(w, kInvalidEntity, levelEntities);
        check(result == kInvalidEntity,
              "a same-named entity outside levelEntities is correctly ignored, not returned");
        w.destroy(foreign);
    }

    AVER_INFO("a stale cached handle with nothing in levelEntities to replace it with yields kInvalidEntity");
    {
        const Entity marker = w.create("PlayerStart");
        w.destroy(marker);
        w.flush();   // retire it for real -- see the flush() comment two cases above
        const std::vector<Entity> levelEntities;
        const Entity result = editor::refreshPlayerStart(w, marker, levelEntities);
        check(result == kInvalidEntity, "no live replacement -> kInvalidEntity, not the dead handle");
    }

    AVER_INFO("a differently-named entity in levelEntities is not mistaken for the marker");
    {
        const Entity notIt = w.create("Meshes/cube.ocmesh");
        const std::vector<Entity> levelEntities = {notIt};
        const Entity result = editor::refreshPlayerStart(w, kInvalidEntity, levelEntities);
        check(result == kInvalidEntity, "a non-matching name does not satisfy the walk");
        w.destroy(notIt);
    }

    AVER_INFO("PlayerStartRefreshTest: {} of {} checks passed", g_checks - g_failures, g_checks);
    return g_failures ? 1 : 0;
}
