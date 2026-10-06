// Cover and squad primitives: protection against a threat, generation beside walls, queries,
// reservation conflicts and expiry, flank/spacing/role maths. Pure: a hand-drawn grid.
#include "SynapseAiTestUtil.hpp"

#include "aver/synapse/Cover.hpp"
#include "aver/synapse/CrowdOrca.hpp"   // navCellBlocked

#include <algorithm>
#include <cmath>

using namespace aver;
using namespace aver::synapse;
using aitest::check;

namespace {

bool approx(f32 a, f32 b, f32 eps = 1e-2f) { return std::fabs(a - b) <= eps; }

// 15 x 7 cells of 50 cm with a solid wall in column 7 (x 350..400, y 0..350).
fmt::OcNavData splitMap() {
    std::vector<std::string> rows;
    for (int y = 0; y < 7; ++y) {
        std::string row(15, '.');
        row[7] = '#';
        rows.push_back(row);
    }
    return aitest::gridFrom(rows);
}

// Test path cost: straight distance, except points north of y 250 are cheap and the one at y 175
// is unreachable.
f32 oddCost(void*, V2 from, V2 to) {
    if (to.y > 250.0f) return 1.0f;
    if (to.y > 150.0f && to.y < 200.0f) return -1.0f;
    return dist2(from, to);
}

} // namespace

int main() {
    AVER_INFO("CoverTest");
    const fmt::OcNavData nav = splitMap();

    AVER_INFO("navSegmentBlocked");
    {
        check(navSegmentBlocked(nav, {100, 175}, {600, 175}), "a segment through the wall is blocked");
        check(!navSegmentBlocked(nav, {100, 175}, {300, 175}), "one that stays on a side is clear");
        check(!navSegmentBlocked(nav, {100, 175}, {100, 175}), "a zero-length segment is clear");
        check(navSegmentBlocked(nav, {600, 60}, {100, 300}), "a diagonal across the wall is blocked");
        check(!navSegmentBlocked(nav, {100, 60}, {330, 300}), "a diagonal that stops short of it is clear");
    }

    AVER_INFO("protection: arc and line of sight");
    {
        CoverMap map;
        const u32 id = map.addAuthored({325, 175}, {1, 0});   // west of the wall, facing it
        const CoverPoint* c = map.find(id);
        check(c != nullptr && c->authored && c->enabled, "an authored point is stored");
        if (c) {
            check(map.protectsFrom(*c, {600, 175}, &nav, nullptr, nullptr), "a threat across the wall is covered against");
            check(!map.protectsFrom(*c, {100, 175}, &nav, nullptr, nullptr), "a threat on the open side is not");
            check(!map.protectsFrom(*c, {325, 330}, &nav, nullptr, nullptr), "a threat off to the side, outside the arc, is not");
            check(map.protectsFrom(*c, {600, 175}, nullptr, nullptr, nullptr), "with no line check, the arc alone decides");
            check(!map.protectsFrom(*c, {325, 175}, &nav, nullptr, nullptr), "a threat on the point itself is not");

            static bool answer = true;
            const BlockedFn blk = [](void* u, V2, V2) { return *static_cast<bool*>(u); };
            check(map.protectsFrom(*c, {600, 175}, nullptr, blk, &answer), "a host line check is honoured (blocked)");
            answer = false;
            check(!map.protectsFrom(*c, {600, 175}, &nav, blk, &answer), "and wins over the grid (clear)");
        }

        CoverMap narrow;
        const CoverPoint* n = narrow.find(narrow.addAuthored({325, 175}, {1, 0}, CoverHeight::Low, 20.0f));
        check(n && !narrow.protectsFrom(*n, {600, 300}, nullptr, nullptr, nullptr),
              "a 20 degree arc rejects a threat 24 degrees off its axis");
        check(n && narrow.protectsFrom(*n, {600, 200}, nullptr, nullptr, nullptr), "and accepts one 5 degrees off");
    }

    AVER_INFO("generation beside walls");
    {
        CoverMap map;
        const u32 authored = map.addAuthored({50, 50}, {0, -1});
        CoverGenParams gp;
        gp.minSpacingCm = 100.0f;
        const u32 n = map.generateFromNav(nav, gp);
        check(n > 0 && map.points().size() == n + 1, "points were generated alongside the authored one");

        bool walkable = true, unit = true, nextToWall = true, generatedFlag = true;
        std::vector<V2> gen;
        for (const CoverPoint& p : map.points()) {
            if (p.authored) continue;
            gen.push_back(p.pos);
            generatedFlag = generatedFlag && !p.authored && p.enabled;
            unit = unit && approx(len2(p.dir), 1.0f, 1e-3f);
            u32 cx = 0, cy = 0;
            const bool in = worldToCell(nav, p.pos.x, p.pos.y, cx, cy);
            const fmt::OcNavCell* cell = in ? nav.at(cx, cy) : nullptr;
            walkable = walkable && cell && (cell->flags & fmt::kOcNavWalkable);
            bool wall = false;
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx)
                    if ((dx || dy) && navCellBlocked(nav, static_cast<i32>(cx) + dx, static_cast<i32>(cy) + dy)) wall = true;
            nextToWall = nextToWall && wall;
        }
        check(walkable, "every generated point stands on a walkable cell");
        check(nextToWall, "and next to a wall");
        check(unit && generatedFlag, "with a unit facing direction, marked generated");

        bool spaced = true;
        for (usize i = 0; i < gen.size(); ++i)
            for (usize j = i + 1; j < gen.size(); ++j) spaced = spaced && dist2(gen[i], gen[j]) >= 100.0f - 1e-3f;
        check(spaced, "no two generated points are closer than the minimum spacing");

        bool westFace = false;
        for (const CoverPoint& p : map.points())
            if (!p.authored && approx(p.pos.x, 325.0f) && p.dir.x > 0.5f) westFace = true;
        check(westFace, "the wall's west face got a point looking at it");

        // Validity: every point that faces the central wall squarely protects against a threat
        // straight across it.
        bool valid = true;
        u32 checked = 0;
        for (const CoverPoint& p : map.points()) {
            if (p.authored) continue;
            if (p.dir.x > 0.9f && p.pos.x < 350.0f)       { valid = valid && map.protectsFrom(p, {p.pos.x + 300.0f, p.pos.y}, &nav, nullptr, nullptr); ++checked; }
            else if (p.dir.x < -0.9f && p.pos.x > 400.0f) { valid = valid && map.protectsFrom(p, {p.pos.x - 300.0f, p.pos.y}, &nav, nullptr, nullptr); ++checked; }
        }
        check(checked > 0 && valid, "each wall-facing generated point protects against a threat across the wall");

        const u32 again = map.generateFromNav(nav, gp);
        check(again == n && map.points().size() == n + 1 && map.find(authored) != nullptr,
              "regenerating replaces the generated points and keeps the authored one");
        map.clearGenerated();
        check(map.points().size() == 1 && map.points()[0].authored, "clearGenerated leaves only authored points");

        CoverGenParams few;
        few.maxPoints = 5;
        few.minSpacingCm = 50.0f;
        check(map.generateFromNav(nav, few) == 5, "maxPoints caps the count");

        CoverGenParams picky;
        picky.isWall = [](void*, V2, V2) { return false; };
        CoverMap none;
        check(none.generateFromNav(nav, picky) == 0, "a wall check that rejects every neighbour yields no points");
    }

    AVER_INFO("queries and reservations");
    {
        CoverMap map;
        const u32 p1 = map.addAuthored({325, 75}, {1, 0});
        const u32 p2 = map.addAuthored({325, 175}, {1, 0});
        const u32 p3 = map.addAuthored({325, 275}, {1, 0});

        CoverQuery q;
        q.from = {300, 175};
        q.threat = {600, 175};
        q.minThreatDistCm = 100.0f;
        q.nav = &nav;
        q.seeker = 1;

        CoverReservations res;
        CoverResult r;
        check(map.claim(q, res, 0.0f, r) && r.id == p2 && approx(r.cost, 25.0f), "the first seeker gets the nearest point");
        check(res.ownerOf(p2) == 1 && res.coverOf(1) == p2, "which is now reserved for it");

        q.seeker = 2;
        q.from = {300, 170};
        check(map.claim(q, res, 0.0f, r) && r.id == p1, "a second seeker is sent to the next nearest, not the same point");
        q.seeker = 3;
        q.from = {300, 280};
        check(map.claim(q, res, 0.0f, r) && r.id == p3, "and a third to the remaining one");
        q.seeker = 4;
        check(!map.claim(q, res, 0.0f, r), "a fourth finds every point taken");
        check(!res.reserve(p1, 9, 0.0f), "reserving a point someone else holds fails");
        check(res.reservedByOther(p1, 9) && !res.reservedByOther(p1, 2), "reservedByOther tells holder from outsider");

        q.seeker = 1;
        q.from = {300, 175};
        check(map.query(q, &res, r) && r.id == p2, "a holder is not blocked by its own reservation");

        res.release(p2, 7);
        check(res.ownerOf(p2) == 1, "release by a non-owner does nothing");
        res.release(p2, 1);
        check(res.ownerOf(p2) == 0, "release by the owner frees the point");
        q.seeker = 4;
        q.from = {300, 175};
        check(map.claim(q, res, 0.0f, r) && r.id == p2, "which the fourth seeker can now take");

        res.release(p3, 3);
        check(res.reserve(p3, 2, 0.0f) && res.coverOf(2) == p3 && res.ownerOf(p1) == 0,
              "an owner holds one point: reserving another moves its claim");
        res.releaseAll(2);
        check(res.coverOf(2) == 0, "releaseAll drops whatever an owner held");

        CoverReservations timed;
        timed.reserve(p1, 5, 2.0f);
        timed.tick(1.0f);
        check(timed.ownerOf(p1) == 5, "a timed claim holds before its time is up");
        timed.tick(1.5f);
        check(timed.ownerOf(p1) == 0, "and expires after");
        timed.reserve(p1, 5, 0.0f);
        timed.tick(1000.0f);
        check(timed.ownerOf(p1) == 5, "a claim with no time limit never expires");
    }

    AVER_INFO("queries: threat side, range, height, enabled flag, path cost");
    {
        CoverMap map;
        const u32 p1 = map.addAuthored({325, 75}, {1, 0}, CoverHeight::Low);
        const u32 p2 = map.addAuthored({325, 175}, {1, 0});
        const u32 p3 = map.addAuthored({325, 275}, {1, 0});
        CoverResult r;

        CoverQuery q;
        q.from = {300, 175};
        q.threat = {600, 175};
        q.minThreatDistCm = 100.0f;
        q.nav = &nav;
        check(map.query(q, nullptr, r) && r.id == p2, "baseline: the nearest point protects");

        CoverQuery same = q;
        same.threat = {100, 175};
        check(!map.query(same, nullptr, r), "a threat on the open side leaves nothing that protects");

        CoverQuery tight = q;
        tight.maxSeekCm = 10.0f;
        check(!map.query(tight, nullptr, r), "nothing within a 10 cm seek range");

        CoverQuery close = q;
        close.minThreatDistCm = 400.0f;
        check(!map.query(close, nullptr, r), "points closer to the threat than the minimum are rejected");

        CoverQuery fwd = q;
        fwd.from = {100, 175};
        fwd.maxCloserCm = 10.0f;
        check(!map.query(fwd, nullptr, r), "a point that brings the seeker much nearer the threat is rejected");

        CoverQuery high = q;
        high.from = {300, 75};
        high.requireHigh = true;
        check(map.query(high, nullptr, r) && r.id != p1, "requireHigh skips the low point even when it is nearest");
        high.requireHigh = false;
        check(map.query(high, nullptr, r) && r.id == p1, "and accepts it otherwise");

        map.setEnabled(p2, false);
        check(map.query(q, nullptr, r) && r.id != p2, "a disabled point is not offered");
        map.setEnabled(p2, true);

        CoverQuery costed = q;
        costed.pathCost = &oddCost;
        check(map.query(costed, nullptr, r) && r.id == p3 && approx(r.cost, 1.0f), "path cost, not distance, picks the point");
        costed.maxSeekCm = 0.5f;
        check(!map.query(costed, nullptr, r), "a path cost over the seek limit rejects it");

        check(map.remove(p3) && map.find(p3) == nullptr && !map.remove(p3), "remove drops a point once");
    }

    AVER_INFO("squads: roles, flank, spacing, support slots");
    {
        const SquadMemberIn m[] = {{10, {500, 0}}, {11, {100, 0}}, {12, {300, 0}}, {13, {900, 0}}, {14, {200, 0}}};
        SquadRole roles[5];
        squadAssignRoles(m, 5, {0, 0}, roles);
        check(roles[1] == SquadRole::Anchor, "the nearest member anchors");
        check(roles[4] == SquadRole::FlankLeft && roles[2] == SquadRole::FlankRight, "the next two flank, left then right");
        check(roles[0] == SquadRole::Support && roles[3] == SquadRole::Support, "the rest support");

        const SquadMemberIn tie[] = {{21, {100, 0}}, {20, {0, 100}}};
        SquadRole tieRoles[2];
        squadAssignRoles(tie, 2, {0, 0}, tieRoles);
        check(tieRoles[1] == SquadRole::Anchor && tieRoles[0] == SquadRole::FlankLeft, "equal distance: the lower id anchors");
        SquadRole one[1];
        squadAssignRoles(tie, 1, {0, 0}, one);
        check(one[0] == SquadRole::Anchor, "a lone member anchors");
        squadAssignRoles(tie, 0, {0, 0}, nullptr);   // must not touch the null output

        const V2 target{0, 0}, centre{1000, 0};
        const V2 right = squadFlankPosition(centre, target, SquadRole::FlankRight, 500.0f, 60.0f);
        const V2 left = squadFlankPosition(centre, target, SquadRole::FlankLeft, 500.0f, 60.0f);
        check(approx(len2(right - target), 500.0f, 0.05f) && approx(len2(left - target), 500.0f, 0.05f),
              "flank points sit on the circle around the target");
        check(approx(right.x, left.x, 0.05f) && approx(right.y, -left.y, 0.05f) && right.y > 0.0f,
              "and mirror each other about the line from the target to the squad");
        check(approx(right.x, 250.0f, 0.05f) && approx(right.y, 433.0127f, 0.05f), "60 degrees round at radius 500");
        const V2 head = squadFlankPosition(centre, target, SquadRole::Anchor, 500.0f, 60.0f);
        check(approx(head.x, 500.0f, 0.05f) && approx(head.y, 0.0f, 0.05f), "a non-flanker is placed straight ahead");

        const V2 close[] = {{30, 0}};
        const V2 push = squadSpacingPush({0, 0}, close, 1, 100.0f);
        check(approx(push.x, -70.0f) && approx(push.y, 0.0f), "a mate 30 cm away pushes 70 cm the other way");
        const V2 farMate[] = {{300, 0}};
        check(approx(len2(squadSpacingPush({0, 0}, farMate, 1, 100.0f)), 0.0f), "a mate outside the spacing pushes nothing");
        const V2 onTop[] = {{0, 0}};
        check(approx(len2(squadSpacingPush({0, 0}, onTop, 1, 100.0f)), 100.0f), "a coincident mate still gets a defined push");

        const V2 s0 = squadSupportPosition({0, 0}, {500, 0}, 300.0f, 0, 200.0f);
        const V2 s1 = squadSupportPosition({0, 0}, {500, 0}, 300.0f, 1, 200.0f);
        const V2 s2 = squadSupportPosition({0, 0}, {500, 0}, 300.0f, 2, 200.0f);
        check(approx(s0.x, -300.0f) && approx(s0.y, 0.0f), "support slot 0 is straight behind the anchor");
        check(approx(s1.x, -300.0f) && approx(s2.x, -300.0f) && approx(s1.y, -s2.y) && approx(std::fabs(s1.y), 200.0f),
              "slots 1 and 2 spread to either side");
    }

    return aitest::g_failures == 0 ? 0 : 1;
}
