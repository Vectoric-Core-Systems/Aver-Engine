// Synapse navigation bake: turning a world into a grid.
//
// The world here is arithmetic -- a floor function and a headroom function, both written out below
// -- so what the test believes the level looks like and what the baker sees cannot drift apart.
// That is the entire reason bakeNav takes function pointers instead of calling aver_phys_raycast:
// a baker that reached physics directly could only be checked by baking a level and looking at it.
#include "aver/synapse/NavBake.hpp"

#include "aver/core/Log.hpp"
#include "aver/synapse/Nav.hpp"

#include <cmath>
#include <string>

using namespace aver;

static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

// ---------------------------------------------------------------------------------------------
// The world. 1000 cm square of flat ground at z = 0, with:
//   - a RAMP over x in [200, 400] rising at 20 degrees (walkable at the 50-degree default),
//   - a CLIFF over x in [600, 800] where the floor jumps to z = 300 (walkable ground, but a step
//     no agent can take, so it should come out as its own region),
//   - a WALL over x in [450, 550] where an agent has no headroom,
//   - a HOLE over y > 900 where there is no floor at all.
// ---------------------------------------------------------------------------------------------
struct World {
    bool wallPresent = true;
};

static bool worldFloor(void* user, f32 x, f32 y, f32 /*topZ*/, f32 /*depth*/,
                       f32* outZ, f32* outNormalZ) {
    (void)user;
    if (y > 900.0f) return false;                     // the hole: nothing under the column
    if (x >= 600.0f && x < 800.0f) { *outZ = 300.0f; *outNormalZ = 1.0f; return true; }   // cliff top
    if (x >= 200.0f && x < 400.0f) {
        // A 20-degree ramp. Its normal's up component is cos(20) -- comfortably inside the limit.
        *outZ = (x - 200.0f) * std::tan(20.0f * 3.14159265f / 180.0f);
        *outNormalZ = std::cos(20.0f * 3.14159265f / 180.0f);
        return true;
    }
    *outZ = 0.0f; *outNormalZ = 1.0f;
    return true;
}

static bool worldHeadroom(void* user, f32 x, f32 /*y*/, f32 /*z*/, f32 /*r*/, f32 /*h*/) {
    const World* w = static_cast<const World*>(user);
    if (w && w->wallPresent && x >= 450.0f && x < 550.0f) return false;
    return true;
}

// A world made entirely of 80-degree slope: nothing should be walkable.
static bool cliffFaceFloor(void*, f32, f32, f32, f32, f32* outZ, f32* outNormalZ) {
    *outZ = 0.0f;
    *outNormalZ = std::cos(80.0f * 3.14159265f / 180.0f);
    return true;
}

// Counts how many times it was asked, so the per-cell probe count can be asserted rather than
// assumed. `user` is the counter.
static bool countingFloor(void* user, f32, f32, f32, f32, f32* outZ, f32* outNormalZ) {
    ++*static_cast<int*>(user);
    *outZ = 0.0f; *outNormalZ = 1.0f;
    return true;
}

int main() {
    AVER_INFO("NavBakeTest");

    synapse::BakeParams p;
    p.originXCm = 0.0f; p.originYCm = 0.0f;
    p.cellSizeCm = 50.0f;
    p.widthCells = 20; p.heightCells = 20;   // 1000 x 1000 cm

    AVER_INFO("the bake refuses what it cannot do, rather than producing a grid nobody can trust");
    {
        fmt::OcNavData nav; std::string why;
        synapse::BakeParams bad = p;
        bad.widthCells = 0;
        check(!synapse::bakeNav(bad, worldFloor, nullptr, nullptr, nav, nullptr, &why),
              "an empty area is refused: " + why);

        bad = p; bad.cellSizeCm = 0.0f;
        check(!synapse::bakeNav(bad, worldFloor, nullptr, nullptr, nav, nullptr, &why),
              "a zero cell size is refused: " + why);

        bad = p; bad.widthCells = 100000; bad.heightCells = 100000;
        check(!synapse::bakeNav(bad, worldFloor, nullptr, nullptr, nav, nullptr, &why),
              "an absurd extent is REFUSED rather than allocated -- the editor derives the extent "
              "from level bounds, and one placement at 1e9 makes those bounds absurd: " + why);

        check(!synapse::bakeNav(p, nullptr, nullptr, nullptr, nav, nullptr, &why),
              "and a bake with no floor probe at all is refused: " + why);
    }

    AVER_INFO("a flat floor bakes walkable, and every cell is probed exactly once");
    {
        int probes = 0;
        fmt::OcNavData nav; synapse::BakeStats st;
        check(synapse::bakeNav(p, countingFloor, nullptr, &probes, nav, &st, nullptr), "it bakes");
        check(probes == 400, "400 cells means 400 floor probes, not 401 and not 800");
        check(st.cellsWalkable == 400, "and all 400 are walkable");
        check(st.regions == 1, "flat open ground is ONE region");
    }

    World w;
    fmt::OcNavData nav;
    synapse::BakeStats st;
    std::string why;
    check(synapse::bakeNav(p, worldFloor, worldHeadroom, &w, nav, &st, &why),
          "the shaped world bakes: " + why);

    AVER_INFO("a hole in the floor is unwalkable, not an error");
    {
        // y > 900 is cells 18 and 19 (centres 925 and 975).
        const fmt::OcNavCell* c = nav.at(0, 19);
        check(c && c->flags == 0, "a column with nothing under it comes out unwalkable");
        check(st.rejectedNoFloor == 40,
              "and the whole two rows are counted as no-floor rather than silently dropped");
        check(nav.at(0, 17) && (nav.at(0, 17)->flags & fmt::kOcNavWalkable) != 0,
              "the row below the hole is unaffected");
    }

    AVER_INFO("a wall an agent cannot fit through is unwalkable");
    {
        // x in [450, 550) is cells 9 and 10 (centres 475 and 525).
        check(nav.at(9, 0) && nav.at(9, 0)->flags == 0, "the wall column is not walkable");
        check(nav.at(10, 0) && nav.at(10, 0)->flags == 0, "nor the next one");
        check(nav.at(8, 0) && (nav.at(8, 0)->flags & fmt::kOcNavWalkable) != 0,
              "but the cell beside it is");
        check(st.rejectedHeadroom > 0 && st.rejectedSlope == 0,
              "and it is counted as HEADROOM, not slope -- the two failures have different fixes "
              "and a bake that conflates them sends somebody to widen a doorway that is fine");
    }

    AVER_INFO("a 20-degree ramp is walkable at a 50-degree limit; an 80-degree face is not");
    {
        check(nav.at(5, 0) && (nav.at(5, 0)->flags & fmt::kOcNavWalkable) != 0,
              "the ramp bakes walkable");
        check(nav.at(5, 0)->floorZCm > 1.0f, "carrying the HEIGHT it was sampled at, not zero");

        fmt::OcNavData steep; synapse::BakeStats sst;
        check(synapse::bakeNav(p, cliffFaceFloor, nullptr, nullptr, steep, &sst, nullptr),
              "a world of 80-degree slope still bakes");
        check(sst.cellsWalkable == 0 && sst.rejectedSlope == 400,
              "and nothing on it is walkable");

        synapse::BakeParams lenient = p;
        lenient.maxSlopeDeg = 85.0f;
        fmt::OcNavData ok; synapse::BakeStats ost;
        check(synapse::bakeNav(lenient, cliffFaceFloor, nullptr, nullptr, ok, &ost, nullptr) &&
              ost.cellsWalkable == 400,
              "raising the limit past it makes the SAME world walkable -- the limit is doing the "
              "work, not the geometry");
    }

    AVER_INFO("THE CLIFF SPLITS THE GRID, which is the thing regions exist to say");
    {
        // x in [600, 800) is cells 12..15, floor at 300 cm -- far over the 40 cm default step.
        check(nav.at(12, 0) && (nav.at(12, 0)->flags & fmt::kOcNavWalkable) != 0,
              "the cliff TOP is walkable ground");
        check(std::fabs(nav.at(12, 0)->floorZCm - 300.0f) < 1e-3f, "at 300 cm");
        check(nav.at(11, 0)->regionId != nav.at(12, 0)->regionId,
              "and it is a DIFFERENT REGION from the ground beside it -- 300 cm is not a step");

        // The wall at cells 9-10 cuts the low ground in two, and the cliff is a third piece.
        check(st.regions >= 3,
              "so the baked level is at least three disconnected pieces, and findPath can refuse "
              "between them without expanding a single node");

        // And the pathfinder must actually agree with the bake, or the regions are decoration.
        const Vec3 a = synapse::cellToWorld(nav, 0, 0);     // low ground, left of the wall
        const Vec3 b = synapse::cellToWorld(nav, 13, 0);    // cliff top
        synapse::PathRequest req;
        req.startXCm = a.x; req.startYCm = a.y; req.goalXCm = b.x; req.goalYCm = b.y;
        req.snapRingCells = 0;
        const synapse::PathResult r = synapse::findPath(nav, req);
        check(r.status == synapse::PathStatus::Unreachable && r.expansions == 0,
              "and findPath refuses that pair in O(1) -- the bake and the search agree");
    }

    AVER_INFO("removing the wall reconnects the level");
    {
        World open; open.wallPresent = false;
        fmt::OcNavData nav2; synapse::BakeStats st2;
        check(synapse::bakeNav(p, worldFloor, worldHeadroom, &open, nav2, &st2, nullptr),
              "it re-bakes with the wall gone");
        check(st2.rejectedHeadroom == 0, "nothing is rejected for headroom any more");
        check(st2.regions == st.regions - 1,
              "and there is exactly ONE FEWER region -- the wall was making one of them");
    }

    AVER_INFO("a baked grid is a saveable grid");
    {
        // buildRegions runs inside bakeNav rather than being left to the caller, and writeOcNav
        // refuses a walkable cell with no region. If the bake ever stopped doing it, this is where
        // that shows up, instead of at the first Save in somebody's editor.
        std::vector<u8> bytes;
        check(nav.valid() && fmt::writeOcNav(nav, bytes, &why),
              "the bake's own output writes without a fix-up step: " + why);
        fmt::OcNavData back;
        check(fmt::parseOcNav(bytes.data(), bytes.size(), back, &why) &&
              back.cells.size() == nav.cells.size(),
              "and reads back: " + why);
        check(std::fabs(back.maxSlopeDeg - p.maxSlopeDeg) < 1e-4f &&
              std::fabs(back.agentRadiusCm - p.agentRadiusCm) < 1e-4f,
              "with the parameters the bake USED, so a runtime mismatch is detectable");
    }

    AVER_INFO(g_failures ? "NavBakeTest: {} FAILURES" : "NavBakeTest: all checks passed ({})", g_failures);
    return g_failures ? 1 : 0;
}
