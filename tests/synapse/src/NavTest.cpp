// Synapse navigation: the grid format, the regions, and the search.
//
// Every grid here is hand-authored in ASCII, so what the test believes about the world and what the
// pathfinder sees cannot drift apart. This is the whole reason Aver.Synapse is pure: no device, no
// physics, no scene, so the arithmetic can be checked against a picture.
#include "aver/synapse/Nav.hpp"

#include "aver/core/Log.hpp"

#include <cmath>
#include <string>
#include <vector>

using namespace aver;

static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

// Builds a grid from ASCII rows. '.' is walkable, '#' is not. Row 0 of `rows` is y = 0.
// Heights are flat unless a row uses a digit, which is that many maxStepCm-and-a-bit above zero.
static fmt::OcNavData gridFrom(const std::vector<std::string>& rows, f32 cell = 50.0f) {
    fmt::OcNavData nav;
    nav.cellSizeCm = cell;
    nav.heightCells = static_cast<u32>(rows.size());
    nav.widthCells = rows.empty() ? 0 : static_cast<u32>(rows[0].size());
    nav.cells.resize(static_cast<usize>(nav.widthCells) * nav.heightCells);
    for (u32 y = 0; y < nav.heightCells; ++y) {
        for (u32 x = 0; x < nav.widthCells; ++x) {
            fmt::OcNavCell& c = nav.cells[static_cast<usize>(y) * nav.widthCells + x];
            const char ch = rows[y][x];
            if (ch == '#') { c.flags = 0; c.floorZCm = 0.0f; continue; }
            c.flags = fmt::kOcNavWalkable;
            // A digit raises the floor by that many 100cm steps -- well over the 40cm default, so a
            // digit is a cliff rather than a kerb.
            c.floorZCm = (ch >= '1' && ch <= '9') ? static_cast<f32>(ch - '0') * 100.0f : 0.0f;
        }
    }
    synapse::buildRegions(nav);
    return nav;
}

// The world position of a cell centre, for building a request.
static void centre(const fmt::OcNavData& nav, u32 x, u32 y, f32& ox, f32& oy) {
    const Vec3 p = synapse::cellToWorld(nav, x, y);
    ox = p.x; oy = p.y;
}

int main() {
    AVER_INFO("NavTest");

    AVER_INFO("the grid maps to the world and back");
    {
        fmt::OcNavData nav = gridFrom({"...", "...", "..."});
        u32 x = 99, y = 99;
        check(synapse::worldToCell(nav, 25.0f, 25.0f, x, y) && x == 0 && y == 0,
              "a point inside cell 0,0 lands in cell 0,0");
        check(synapse::worldToCell(nav, 75.0f, 125.0f, x, y) && x == 1 && y == 2,
              "and a point two cells up lands two cells up");
        check(!synapse::worldToCell(nav, -1.0f, 25.0f, x, y),
              "a point LEFT of the grid is off it -- not clamped to column 0");
        check(!synapse::worldToCell(nav, 1000.0f, 25.0f, x, y), "and one past the right edge too");

        const Vec3 c = synapse::cellToWorld(nav, 1, 1);
        check(std::fabs(c.x - 75.0f) < 1e-3f && std::fabs(c.y - 75.0f) < 1e-3f,
              "a cell maps back to its CENTRE, not its corner");
    }

    AVER_INFO("a wall makes two regions, and a path goes around it");
    {
        //  y=4  . . . . .
        //  y=3  . . # . .
        //  y=2  . . # . .
        //  y=1  . . # . .
        //  y=0  . . . . .     <- the gap along the bottom
        fmt::OcNavData nav = gridFrom({
            ".....",
            "..#..",
            "..#..",
            "..#..",
            "....."});
        check(synapse::buildRegions(nav) == 1,
              "a wall with a way around it is still ONE region -- reachability, not line of sight");

        f32 sx, sy, gx, gy;
        centre(nav, 0, 3, sx, sy);
        centre(nav, 4, 3, gx, gy);
        synapse::PathRequest req;
        req.startXCm = sx; req.startYCm = sy; req.goalXCm = gx; req.goalYCm = gy;
        const synapse::PathResult r = synapse::findPath(nav, req);
        check(r.status == synapse::PathStatus::Found, "a path across the wall is found");
        check(r.points.size() >= 3,
              "and it BENDS -- a straight two-point path would be one through the wall");

        // Every leg must be clear. A string-pull that shortcuts a corner produces a path that looks
        // plausible and walks an agent into geometry.
        bool clear = true;
        for (usize i = 0; i + 1 < r.points.size(); ++i) {
            u32 ax, ay, bx, by;
            if (!synapse::worldToCell(nav, r.points[i].x, r.points[i].y, ax, ay) ||
                !synapse::worldToCell(nav, r.points[i + 1].x, r.points[i + 1].y, bx, by) ||
                !synapse::lineOfSight(nav, ax, ay, bx, by)) clear = false;
        }
        check(clear, "and EVERY leg of the pulled path is in clear line of sight");
    }

    AVER_INFO("a sealed room is refused in O(1), without searching");
    {
        //  A room walled off completely from the rest of the grid.
        fmt::OcNavData nav = gridFrom({
            ".......",
            ".......",
            "..###..",
            "..#.#..",
            "..###..",
            ".......",
            "......."});
        check(synapse::buildRegions(nav) == 2, "the sealed cell is its own region");

        f32 sx, sy, gx, gy;
        centre(nav, 0, 0, sx, sy);
        centre(nav, 3, 3, gx, gy);     // inside the room
        synapse::PathRequest req;
        req.startXCm = sx; req.startYCm = sy; req.goalXCm = gx; req.goalYCm = gy;
        req.snapRingCells = 0;         // no snapping, or it would escape the room
        const synapse::PathResult r = synapse::findPath(nav, req);
        check(r.status == synapse::PathStatus::Unreachable, "the query is Unreachable");
        check(r.expansions == 0,
              "AND IT EXPANDED NOTHING -- the region check refused before A* ran, which is the "
              "whole reason regions are baked");
        check(r.points.empty(), "with no path handed back");
    }

    AVER_INFO("a diagonal cannot squeeze through a corner");
    {
        //  # .        A gap between two blocked cells that touch only at a corner.
        //  . #
        fmt::OcNavData nav = gridFrom({
            "#.",
            ".#"});
        check(!synapse::canStep(nav, 0, 1, 1, 0),
              "stepping diagonally between two blocked corners is refused");
        check(synapse::buildRegions(nav) == 2,
              "and the two open cells are therefore SEPARATE regions -- the flood uses the same "
              "rule the search does, or the O(1) check would promise a path A* cannot find");
    }

    AVER_INFO("a cliff is not a step");
    {
        // Flat ground, then a column raised 300cm -- far over the 40cm default step.
        fmt::OcNavData nav = gridFrom({
            "..3..",
            "..3..",
            "..3..",
            "..3..",
            "..3.."});
        check(synapse::buildRegions(nav) == 3,
              "the raised strip is its own region, and the two flat sides are separate");
        check(!synapse::canStep(nav, 1, 2, 2, 2), "stepping up the cliff is refused");

        nav.maxStepCm = 400.0f;
        synapse::buildRegions(nav);
        check(synapse::buildRegions(nav) == 1,
              "and with a step limit that CAN cross it, the whole grid is one region again");
    }

    AVER_INFO("an off-grid endpoint is reported, not clamped");
    {
        fmt::OcNavData nav = gridFrom({"...", "...", "..."});
        synapse::PathRequest req;
        req.startXCm = -500.0f; req.startYCm = -500.0f;
        req.goalXCm = 75.0f;    req.goalYCm = 75.0f;
        const synapse::PathResult r = synapse::findPath(nav, req);
        check(r.status == synapse::PathStatus::OffMesh,
              "a start far outside the grid is OffMesh rather than silently clamped to the rim");
    }

    AVER_INFO("a start ON an unwalkable cell snaps to the nearest walkable one");
    {
        fmt::OcNavData nav = gridFrom({
            ".....",
            ".....",
            "..#..",
            ".....",
            "....."});
        f32 sx, sy, gx, gy;
        centre(nav, 2, 2, sx, sy);     // the blocked cell itself
        centre(nav, 4, 4, gx, gy);
        synapse::PathRequest req;
        req.startXCm = sx; req.startYCm = sy; req.goalXCm = gx; req.goalYCm = gy;
        const synapse::PathResult r = synapse::findPath(nav, req);
        check(r.status == synapse::PathStatus::Found,
              "an agent standing in a wall can still be given a path out of it");
    }

    AVER_INFO("the budget bounds the search rather than the frame");
    {
        // Big and open: a long path that would expand a lot of nodes.
        std::vector<std::string> rows(60, std::string(60, '.'));
        fmt::OcNavData nav = gridFrom(rows);
        f32 sx, sy, gx, gy;
        centre(nav, 0, 0, sx, sy);
        centre(nav, 59, 59, gx, gy);
        synapse::PathRequest req;
        req.startXCm = sx; req.startYCm = sy; req.goalXCm = gx; req.goalYCm = gy;
        req.maxExpansions = 20;
        const synapse::PathResult r = synapse::findPath(nav, req);
        check(r.expansions <= 21,
              "a tiny budget stops the search -- it does not run to completion anyway");
        check(r.status == synapse::PathStatus::Partial, "and the result says so");
        check(!r.points.empty(),
              "but a PARTIAL path is still handed back, so an agent moves toward the goal and asks "
              "again rather than standing still");
    }

    AVER_INFO("the grid round-trips through .ocnav");
    {
        fmt::OcNavData nav = gridFrom({
            "..#..",
            "..#..",
            "....."});
        nav.originXCm = -1234.5f;
        nav.originYCm = 678.25f;
        nav.agentRadiusCm = 40.0f;
        nav.maxStepCm = 33.0f;

        std::vector<u8> bytes; std::string why;
        check(fmt::writeOcNav(nav, bytes, &why), "it writes: " + why);
        fmt::OcNavData back;
        check(fmt::parseOcNav(bytes.data(), bytes.size(), back, &why), "and reads: " + why);
        check(back.widthCells == nav.widthCells && back.heightCells == nav.heightCells,
              "the dimensions survive");
        check(std::fabs(back.originXCm + 1234.5f) < 1e-3f, "a NEGATIVE origin survives");
        check(std::fabs(back.maxStepCm - 33.0f) < 1e-4f,
              "and the bake's own parameters survive, so a mismatch is detectable later");
        bool same = back.cells.size() == nav.cells.size();
        for (usize i = 0; same && i < nav.cells.size(); ++i)
            if (back.cells[i].flags != nav.cells[i].flags ||
                back.cells[i].regionId != nav.cells[i].regionId) same = false;
        check(same, "every cell's walkability and region survive");

        std::vector<u8> again;
        check(fmt::writeOcNav(back, again, &why), "the parsed grid writes again");
        check(again == bytes, "BYTE-IDENTICAL after a parse and a rewrite");

        // The invariant the reader refuses on: walkable and region must agree, because the O(1)
        // reachability check is built entirely on them agreeing.
        fmt::OcNavData bad = nav;
        bad.cells[0].regionId = 0;    // walkable, but claims no region
        std::vector<u8> junk;
        check(!bad.valid() && !fmt::writeOcNav(bad, junk, &why),
              "a walkable cell with no region is refused at write");
    }

    AVER_INFO(g_failures ? "NavTest: {} FAILURES" : "NavTest: all checks passed ({})", g_failures);
    return g_failures ? 1 : 0;
}
