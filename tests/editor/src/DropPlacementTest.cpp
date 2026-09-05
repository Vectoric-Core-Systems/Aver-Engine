// DropPlacementTest -- an asset dragged onto something in the viewport rests ON it.
//
// WHAT THIS IS FOR. Dropping a mesh from the Content Browser put the new entity's ORIGIN at the point
// the mouse ray met a surface. For a mesh authored around its own centre that buries the lower half in
// whatever it landed on -- and because the drop ray tests an entity's BOUNDING BOX, dropping onto
// another object put the new one inside the old one's silhouette. Reported as "drag and drop replaces
// the object": nothing was replaced, the two were simply occupying the same space.
//
// WHY A TEST FOR SIX LINES OF ARITHMETIC. The lift is not hard, it is EASY TO GET BACKWARDS -- and a
// sign error here does not crash or log, it drops objects through the floor, which is exactly the
// class of bug the editor's Euler pair was written to stop repeating (see EditorEulerTest's own
// header). It is also the only part of the drop path reachable without a device, a window and a
// physical drag, so pinning it here is the difference between a rule that is checked and a rule that
// is merely intended.
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

    AVER_INFO("=== {} assertions, {} failed ===", g_checks, g_failures);
    return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
}
