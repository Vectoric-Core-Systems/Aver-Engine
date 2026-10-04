// DropPlacementTest -- the two pieces of the drag-and-drop gesture a headless suite can reach.
//
// WHAT THIS IS FOR. Dropping a mesh from the Content Browser put the new entity's ORIGIN at the point
// the mouse ray met a surface. For a mesh authored around its own centre that buries the lower half in
// whatever it landed on -- and because the drop ray tests an entity's BOUNDING BOX, dropping onto
// another object put the new one inside the old one's silhouette. Reported as "drag and drop replaces
// the object": nothing was replaced, the two were simply occupying the same space.
//
// WHY A TEST FOR THIS ARITHMETIC. It is not hard, it is EASY TO GET BACKWARDS -- and a sign error here
// does not crash or log, it drops objects through the floor, which is exactly the class of bug the
// editor's Euler pair was written to stop repeating (see EditorEulerTest's own header). These are also
// the only parts of the drop path reachable without a device, a window and a physical drag, so pinning
// them here is the difference between a rule that is checked and a rule that is merely intended.
#include "EditorTransform.hpp"
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"

#include <cmath>
#include <string>

using namespace aver;

static int g_checks = 0;
static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

static bool nearly(f32 a, f32 b) { return std::fabs(a - b) < 1e-4f; }

int main() {
    AVER_INFO("=== a dropped asset rests on what it landed on ===");

    // THE CASE THE REPORT WAS ABOUT: a mesh authored around its own middle. Half of it is below the
    // origin, so putting the origin on the surface buries that half. The lift is exactly that half.
    check(nearly(editor::dropRestLift(-50.0f, 1.0f), 50.0f),
          "a mesh centred on its origin is lifted by its own half-height");

    // THE CASE THAT MUST NOT MOVE, and it is most of them: foliage is authored standing on its origin,
    // so its lowest point IS the origin and the surface is already where it belongs. A lift here would
    // float every plant in the project above the ground for no reason a person could see.
    check(nearly(editor::dropRestLift(0.0f, 1.0f), 0.0f),
          "a mesh already authored on its base is not moved at all");

    // SCALE COUNTS. The bounds are LOCAL to the mesh; a half-scale object sinks half as far, and
    // lifting by the unscaled figure would leave it hovering.
    check(nearly(editor::dropRestLift(-50.0f, 0.5f), 25.0f),
          "the lift scales with the object");
    check(nearly(editor::dropRestLift(-50.0f, 2.0f), 100.0f),
          "  in both directions");

    // A ZERO SCALE IS NOT A ZERO LIFT. A degenerate transform is a bug somewhere else; treating its
    // scale as 1 keeps this function from quietly multiplying the answer away and hiding it.
    check(nearly(editor::dropRestLift(-50.0f, 0.0f), 50.0f),
          "a zero scale is treated as 1 rather than collapsing the lift");

    // NEVER NEGATIVE, which is the sign error this test exists for. A mesh authored entirely ABOVE
    // its origin is already clear of the surface; pulling it DOWN into the thing it was dropped on
    // would be the same bug the lift was written to fix, in the other direction.
    check(nearly(editor::dropRestLift(10.0f, 1.0f), 0.0f),
          "a mesh authored above its origin is never pulled DOWN into the surface");
    check(nearly(editor::dropRestLift(1000.0f, 3.0f), 0.0f),
          "  however far above, and whatever the scale");

    // ---- the multi-asset drag payload ------------------------------------------------------------
    //
    // A Content Browser drag now carries EVERY selected placeable asset, one path per line, and the
    // viewport places each. Worth pinning because a mistake here is silent in both directions: split
    // too eagerly and paths become fragments that resolve to nothing, split too little and only the
    // first of eleven assets is placed, with no warning either way.
    AVER_INFO("=== a drag payload carries one path per line ===");

    check(editor::splitDropPayload("Content/Meshes/a.ocmesh").size() == 1,
          "a single-asset drag is one path, exactly as before multi-selection existed");
    check(editor::splitDropPayload("Content/Meshes/a.ocmesh")[0] == "Content/Meshes/a.ocmesh",
          "  and it is returned unchanged");

    {
        const auto many = editor::splitDropPayload("a.ocmesh\nb.ocmesh\nc.ocmesh");
        check(many.size() == 3, "three lines are three assets");
        check(many.size() == 3 && many[0] == "a.ocmesh" && many[1] == "b.ocmesh" && many[2] == "c.ocmesh",
              "  in the order they were dragged");
    }

    // A TRAILING NEWLINE IS THE EASIEST THING FOR A PRODUCER TO ADD, and an empty path reaching the
    // placement code would log "could not resolve" for an asset nobody dragged.
    check(editor::splitDropPayload("a.ocmesh\n").size() == 1,
          "a trailing newline does not invent an empty asset");
    check(editor::splitDropPayload("a.ocmesh\n\nb.ocmesh").size() == 2,
          "nor does a blank line between two");
    check(editor::splitDropPayload("").empty(), "an empty payload places nothing at all");

    // Windows paths, because that is what this editor actually hands it: a backslash is an ordinary
    // character here and must not be mistaken for a separator.
    {
        const auto w = editor::splitDropPayload("C:\\Proj\\Content\\a.ocmesh\nC:\\Proj\\Content\\b.ocmesh");
        check(w.size() == 2, "a two-asset drag of drive-letter paths splits into two");
        check(w.size() == 2 && w[0] == "C:\\Proj\\Content\\a.ocmesh",
              "  and each path keeps every backslash it arrived with");
    }

    AVER_INFO("=== {} assertions, {} failed ===", g_checks, g_failures);
    return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
}
