// AStarGraph (AStar3D / AStar2D) and AStarGrid (AStarGrid2D) against paths worked out by hand, plus a jump-point search that must cost
// exactly what the plain search costs on random grids.
#include "aver/core/Log.hpp"
#include "aver/synapse/NavGraph.hpp"

#include <cmath>
#include <string>
#include <vector>

using namespace aver;
using namespace aver::synapse;

static int g_failures = 0;
static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}
static bool near(f32 a, f32 b, f32 eps = 1e-3f) { return std::fabs(a - b) <= eps; }
using Ids = std::vector<i64>;
using Cells = std::vector<std::pair<i32, i32>>;

static f32 octilePathCost(const Cells& p) {
    f32 c = 0;
    for (usize i = 1; i < p.size(); ++i) c += AStarGrid::heuristic(AStarGrid::Octile, p[i].first - p[i - 1].first, p[i].second - p[i - 1].second);
    return c;
}

static u32 lcg(u32& s) { s = s * 1664525u + 1013904223u; return s >> 8; }

int main() {
    AVER_INFO("NavGraphTest");

    AVER_INFO("AStarGraph: paths");
    {
        AStarGraph g;
        g.addPoint(0, {0, 0, 0}); g.addPoint(1, {1, 0, 0}); g.addPoint(2, {2, 0, 0}); g.addPoint(3, {3, 0, 0});
        g.connect(0, 1); g.connect(1, 2); g.connect(2, 3);
        check(g.idPath(0, 3) == Ids({0, 1, 2, 3}), "a line");
        check(g.idPath(3, 0) == Ids({3, 2, 1, 0}), "and back (bidirectional)");
        check(g.idPath(2, 2) == Ids({2}), "from == to is the point itself");
        check(g.idPath(0, 9).empty() && g.idPath(9, 0).empty(), "a missing point gives nothing");
        const auto pts = g.pointPath(0, 2);
        check(pts.size() == 3 && near(pts[2].x, 2), "point path");
        check(g.pointCount() == 4 && g.pointIds() == Ids({0, 1, 2, 3}), "ids");
        check(g.nextFreeId() == 4, "next free id");
        g.removePoint(2);
        check(g.idPath(0, 3).empty() && !g.areConnected(1, 2), "removing a point cuts its connections");
        check(g.nextFreeId() == 2, "the smallest unused id");
        g.addPoint(-5, {});
        check(!g.hasPoint(-5), "negative ids are refused");
    }
    {
        // A diamond: 0 -> {1, 2} -> 3. Weights decide which way round.
        AStarGraph g;
        g.addPoint(0, {0, 0, 0}); g.addPoint(1, {1, 1, 0}, 3.0f); g.addPoint(2, {1, -1, 0}); g.addPoint(3, {2, 0, 0});
        g.connect(0, 1); g.connect(0, 2); g.connect(1, 3); g.connect(2, 3);
        check(g.idPath(0, 3) == Ids({0, 2, 3}), "weight 3 on point 1 sends the path through 2");
        g.setPointWeight(1, 1.0f);
        g.setPointWeight(2, 4.0f);
        check(g.idPath(0, 3) == Ids({0, 1, 3}), "and the other way when 2 is heavy");
        g.setPointWeight(2, 1.0f);
        g.setDisabled(1, true);
        check(g.idPath(0, 3) == Ids({0, 2, 3}) && g.isDisabled(1), "a disabled point is not entered");
        g.setDisabled(2, true);
        check(g.idPath(0, 3).empty(), "all routes disabled");
        g.setDisabled(2, false); g.setDisabled(1, false);
        g.setDisabled(0, true);
        check(g.idPath(0, 3).empty(), "a disabled start finds nothing");
        g.setDisabled(0, false); g.setDisabled(3, true);
        check(g.idPath(0, 3).empty(), "a disabled goal finds nothing");
        const Ids partial = g.idPath(0, 3, true);
        check(!partial.empty() && partial.front() == 0 && partial.back() != 3, "a disabled goal with a partial path ends at the nearest reachable point");
    }
    {
        AStarGraph g;
        g.addPoint(0, {0, 0, 0}); g.addPoint(1, {1, 0, 0}); g.addPoint(5, {10, 0, 0});
        g.connect(0, 1);
        check(g.idPath(0, 5).empty(), "unreachable: empty");
        check(g.idPath(0, 5, true) == Ids({0, 1}), "partial: the closest reachable point to the goal");
        check(g.idPath(5, 5, true) == Ids({5}), "partial from == to");
        // One-way segments.
        AStarGraph h;
        h.addPoint(0, {0, 0, 0}); h.addPoint(1, {1, 0, 0});
        h.connect(0, 1, false);
        check(h.idPath(0, 1) == Ids({0, 1}) && h.idPath(1, 0).empty(), "one way");
        check(h.areConnected(0, 1, false) && !h.areConnected(1, 0, false) && h.areConnected(1, 0, true), "are_points_connected with and without bidirectional");
        check(h.connections(0) == Ids({1}) && h.connections(1).empty(), "connections are outgoing");
        h.connect(1, 0, false);
        h.disconnect(0, 1, false);
        check(h.idPath(1, 0) == Ids({1, 0}) && h.idPath(0, 1).empty(), "disconnect one direction leaves the other");
        h.connect(0, 1);
        h.disconnect(0, 1);
        check(!h.areConnected(0, 1) && !h.areConnected(1, 0), "disconnect both");
        check(!h.connect(0, 0) && !h.connect(0, 99), "self and missing connections are refused");
    }
    {
        // Script overrides.
        AStarGraph g;
        g.addPoint(0, {0, 0, 0}); g.addPoint(1, {1, 1, 0}); g.addPoint(2, {2, 0, 0});
        g.connect(0, 1); g.connect(1, 2); g.connect(0, 2);
        check(g.idPath(0, 2) == Ids({0, 2}), "the direct edge is shortest by distance");
        int calls = 0;
        g.setComputeCost([&](i64 a, i64 b) { ++calls; return (a == 0 && b == 2) ? 100.0f : 1.0f; });
        g.setEstimateCost([](i64, i64) { return 0.0f; });
        check(g.idPath(0, 2) == Ids({0, 1, 2}) && calls > 0, "_compute_cost makes the direct edge dear");
        g.setComputeCost(nullptr);
        g.setEstimateCost(nullptr);
        check(g.idPath(0, 2) == Ids({0, 2}), "back to the defaults");
        check(g.closestPoint({1.9f, 0.2f, 0}) == 2, "closest point");
        g.setDisabled(2, true);
        check(g.closestPoint({1.9f, 0.2f, 0}) == 1 && g.closestPoint({1.9f, 0.2f, 0}, true) == 2, "closest point honours disabled");
        g.setDisabled(2, false);
        AStarGraph t;
        t.addPoint(3, {1, 0, 0}); t.addPoint(7, {1, 0, 0});
        check(t.closestPoint({0, 0, 0}) == 3, "ties go to the smaller id");
        AStarGraph s;
        s.addPoint(0, {0, 0, 0}); s.addPoint(1, {10, 0, 0});
        s.connect(0, 1);
        const Vec3 c = s.closestPositionInSegment({4, 5, 0});
        check(near(c.x, 4) && near(c.y, 0), "closest position in a segment");
    }

    AVER_INFO("AStarGrid: diagonal modes and heuristics");
    {
        AStarGrid g;
        g.setRegion({0, 0, 5, 5});
        g.update();
        check(!g.isDirty(), "update clears the dirty flag");
        Cells p = g.idPath(0, 0, 4, 4);
        check(p == Cells({{0, 0}, {1, 1}, {2, 2}, {3, 3}, {4, 4}}), "diagonal mode ALWAYS: straight along the diagonal");
        g.setDiagonalMode(AStarGrid::Never);
        g.setComputeHeuristic(AStarGrid::Manhattan);
        g.setEstimateHeuristic(AStarGrid::Manhattan);
        p = g.idPath(0, 0, 4, 4);
        bool orth = p.size() == 9;
        for (usize i = 1; i < p.size(); ++i) if (std::abs(p[i].first - p[i - 1].first) + std::abs(p[i].second - p[i - 1].second) != 1) orth = false;
        check(orth && p.front() == std::make_pair(0, 0) && p.back() == std::make_pair(4, 4), "NEVER: nine cells, every step orthogonal");
    }
    {
        // A wall at x = 2 with its only gap at y = 4.
        AStarGrid g;
        g.setRegion({0, 0, 5, 5});
        g.update();
        for (int y = 0; y < 4; ++y) g.setSolid(2, y, true);
        g.setComputeHeuristic(AStarGrid::Octile);
        g.setEstimateHeuristic(AStarGrid::Octile);
        Cells p = g.idPath(0, 0, 4, 0);
        check(p.size() == 9, "ALWAYS: through the gap in eight steps");
        bool hasGap = false;
        for (auto& c : p) if (c == std::make_pair(2, 4)) hasGap = true;
        check(hasGap, "ALWAYS: the path goes through (2, 4)");
        check(near(octilePathCost(p), 2 * (4.0f + 2 * 0.41421356f), 0.01f), "ALWAYS: cost 2 x (4 + 2 x 0.414)");
        g.setDiagonalMode(AStarGrid::OnlyIfNoObstacles);
        p = g.idPath(0, 0, 4, 0);
        hasGap = false;
        for (auto& c : p) if (c == std::make_pair(2, 4)) hasGap = true;
        bool cuts = false;   // a diagonal move whose neighbours include a solid cell
        for (usize i = 1; i < p.size(); ++i) {
            const int dx = p[i].first - p[i - 1].first, dy = p[i].second - p[i - 1].second;
            if (dx != 0 && dy != 0 && (g.isSolid(p[i - 1].first + dx, p[i - 1].second) || g.isSolid(p[i - 1].first, p[i - 1].second + dy))) cuts = true;
        }
        check(hasGap && !cuts && octilePathCost(p) > 9.6f, "ONLY_IF_NO_OBSTACLES: never cuts a solid corner, and costs more");
        g.setDiagonalMode(AStarGrid::AtLeastOneWalkable);
        p = g.idPath(0, 0, 4, 0);
        cuts = false;   // both neighbours solid
        for (usize i = 1; i < p.size(); ++i) {
            const int dx = p[i].first - p[i - 1].first, dy = p[i].second - p[i - 1].second;
            if (dx != 0 && dy != 0 && g.isSolid(p[i - 1].first + dx, p[i - 1].second) && g.isSolid(p[i - 1].first, p[i - 1].second + dy)) cuts = true;
        }
        check(!p.empty() && !cuts, "AT_LEAST_ONE_WALKABLE: no diagonal between two solid cells");
        g.setDiagonalMode(AStarGrid::Never);
        p = g.idPath(0, 0, 4, 0);
        check(p.size() == 13, "NEVER: 4 down, 4 across, 4 up");
    }
    {
        AStarGrid g;
        g.setRegion({0, 0, 5, 3});
        g.update();
        g.setDiagonalMode(AStarGrid::Never);
        g.setComputeHeuristic(AStarGrid::Manhattan);
        g.setEstimateHeuristic(AStarGrid::Manhattan);
        for (int x = 1; x <= 3; ++x) g.setWeight(x, 1, 10.0f);
        const Cells p = g.idPath(0, 1, 4, 1);
        check(p.size() == 7, "weights: round the heavy cells (6 steps)");
        bool heavy = false;
        for (auto& c : p) if (c.second == 1 && c.first >= 1 && c.first <= 3) heavy = true;
        check(!heavy, "and none of them is entered");
        g.fillWeight({1, 1, 3, 1}, 1.0f);
        check(g.weight(2, 1) == 1.0f && g.idPath(0, 1, 4, 1).size() == 5, "fill_weight_scale restores the straight line");
        g.fillSolid({1, 0, 3, 3}, true);
        check(g.idPath(0, 1, 4, 1).empty() && g.isSolid(2, 2), "fill_solid_region walls it off");
        const Cells part = g.idPath(0, 1, 4, 1, true);
        check(part == Cells({{0, 1}}), "a partial path stops at the closest cell it could reach");
        g.fillSolid({1, 0, 3, 3}, false);
        g.setSolid(0, 1, true);
        check(g.idPath(0, 1, 0, 1).empty() && g.idPath(0, 1, 4, 1, true).empty(), "a solid start returns an empty path, even from == to or with partial");
        g.setSolid(0, 1, false);
        g.setSolid(4, 1, true);
        check(g.idPath(0, 1, 4, 1).empty(), "a solid goal returns nothing...");
        check(!g.idPath(0, 1, 4, 1, true).empty(), "...unless the path may be partial");
        check(g.idPath(0, 1, 9, 9).empty(), "out of bounds");
    }
    {
        AStarGrid g;
        g.setRegion({0, 0, 4, 4});
        g.setCellSize(2, 3);
        g.setOffset(10, 20);
        check(g.isDirty() && g.idPath(0, 0, 1, 1).empty(), "a dirty grid answers nothing");
        g.update();
        const auto pp = g.pointPath(1, 2, 1, 2);
        check(pp.size() == 1 && near(pp[0].first, 12) && near(pp[0].second, 26), "point path = offset + cell * cell_size");
        const auto path = g.pointPath(0, 0, 2, 0);
        check(path.size() == 3 && near(path[2].first, 14) && near(path[2].second, 20), "path positions");
        g.setSolid(1, 1, true);
        g.update();
        check(!g.isSolid(1, 1), "update() clears the point data");
        // A shifted region: absolute ids.
        AStarGrid h;
        h.setRegion({5, 5, 3, 3});
        h.update();
        check(h.inBounds(5, 5) && !h.inBounds(4, 5) && h.idPath(5, 5, 7, 7) == Cells({{5, 5}, {6, 6}, {7, 7}}), "regions keep absolute ids");
        h.setCellSize(1, 1);
        h.update();
        check(near(h.pointPosition(6, 7).first, 6) && near(h.pointPosition(6, 7).second, 7), "position uses the absolute cell");
        // Overrides.
        AStarGrid o;
        o.setRegion({0, 0, 3, 1});
        o.update();
        o.setComputeCost([](i32, i32, i32 tx, i32) { return tx == 1 ? 100.0f : 1.0f; });
        check(o.idPath(0, 0, 2, 0) == Cells({{0, 0}, {1, 0}, {2, 0}}), "the only way is still the only way");
        o.setRegion({0, 0, 3, 2});
        o.update();
        o.setDiagonalMode(AStarGrid::Never);
        o.setComputeCost([](i32, i32, i32 tx, i32 ty) { return (tx == 1 && ty == 0) ? 100.0f : 1.0f; });
        o.setEstimateCost([](i32, i32, i32, i32) { return 0.0f; });
        const Cells c = o.idPath(0, 0, 2, 0);
        check(c.size() == 5 && c[1] == std::make_pair(0, 1), "_compute_cost steers round the dear cell");
    }

    AVER_INFO("AStarGrid: jump point search");
    {
        AStarGrid g;
        g.setRegion({0, 0, 5, 5});
        g.update();
        g.setComputeHeuristic(AStarGrid::Octile);
        g.setEstimateHeuristic(AStarGrid::Octile);
        g.setJumping(true);
        check(g.idPath(0, 0, 4, 4) == Cells({{0, 0}, {4, 4}}), "open grid, diagonal goal: start and goal only");
        check(g.idPath(0, 0, 4, 2) == Cells({{0, 0}, {2, 2}, {4, 2}}), "open grid: one diagonal run, one straight run");
        g.setDiagonalMode(AStarGrid::Never);
        g.setComputeHeuristic(AStarGrid::Manhattan);
        g.setEstimateHeuristic(AStarGrid::Manhattan);
        check(g.idPath(0, 0, 4, 2) == Cells({{0, 0}, {0, 2}, {4, 2}}), "NEVER: down, then across");
    }
    for (int mode = 0; mode < 4; ++mode) {
        u32 seed = 12345u + static_cast<u32>(mode);
        int compared = 0, agreed = 0, reachableAgree = 0, reachableTotal = 0;
        for (int round = 0; round < 60; ++round) {
            AStarGrid plain, jps;
            for (AStarGrid* g : {&plain, &jps}) {
                g->setRegion({0, 0, 14, 12});
                g->update();
                g->setDiagonalMode(static_cast<AStarGrid::Diagonal>(mode));
                const AStarGrid::Heuristic h = mode == AStarGrid::Never ? AStarGrid::Manhattan : AStarGrid::Octile;
                g->setComputeHeuristic(h);
                g->setEstimateHeuristic(h);
            }
            jps.setJumping(true);
            u32 s2 = seed + static_cast<u32>(round) * 7919u;
            for (int y = 0; y < 12; ++y)
                for (int x = 0; x < 14; ++x)
                    if (lcg(s2) % 100 < 28) { plain.setSolid(x, y, true); jps.setSolid(x, y, true); }
            plain.setSolid(0, 0, false); jps.setSolid(0, 0, false);
            plain.setSolid(13, 11, false); jps.setSolid(13, 11, false);
            const Cells a = plain.idPath(0, 0, 13, 11);
            const Cells b = jps.idPath(0, 0, 13, 11);
            ++compared;
            if (a.empty() != b.empty()) continue;
            ++reachableTotal;
            if (a.empty()) { ++agreed; continue; }
            // The plain cost sums the real steps; the jump path's legs are straight or diagonal-then-straight, so octile (Manhattan for NEVER) is exact.
            const AStarGrid::Heuristic h = mode == AStarGrid::Never ? AStarGrid::Manhattan : AStarGrid::Octile;
            f32 ca = 0, cb = 0;
            for (usize i = 1; i < a.size(); ++i) ca += AStarGrid::heuristic(h, a[i].first - a[i - 1].first, a[i].second - a[i - 1].second);
            for (usize i = 1; i < b.size(); ++i) cb += AStarGrid::heuristic(h, b[i].first - b[i - 1].first, b[i].second - b[i - 1].second);
            ++reachableAgree;
            if (std::fabs(ca - cb) < 1e-3f) ++agreed;
            else if (mode == 0 && compared - agreed <= 2) {
                std::string dump = "mismatch plain " + std::to_string(ca) + " jps " + std::to_string(cb) + "\n";
                for (int y = 0; y < 12; ++y) { for (int x = 0; x < 14; ++x) dump += plain.isSolid(x, y) ? '#' : '.'; dump += '\n'; }
                dump += "plain:"; for (auto& c : a) dump += " (" + std::to_string(c.first) + "," + std::to_string(c.second) + ")";
                dump += "\njps:"; for (auto& c : b) dump += " (" + std::to_string(c.first) + "," + std::to_string(c.second) + ")";
                AVER_INFO("{}", dump);
            }
        }
        const std::string name = mode == 0 ? "ALWAYS" : mode == 1 ? "NEVER" : mode == 2 ? "AT_LEAST_ONE_WALKABLE" : "ONLY_IF_NO_OBSTACLES";
        if (mode <= 1) check(agreed == compared, "jump search costs what the plain search costs on random grids, " + name + " (" + std::to_string(agreed) + "/" + std::to_string(compared) + ")");
        else check(reachableAgree == 0 || agreed * 10 >= reachableTotal * 9, "jump search stays within 10% of optimal on random grids, " + name + " (" + std::to_string(agreed) + "/" + std::to_string(reachableTotal) + ")");
    }

    AVER_INFO(g_failures == 0 ? "NavGraphTest passed" : "NavGraphTest FAILED");
    return g_failures == 0 ? 0 : 1;
}
