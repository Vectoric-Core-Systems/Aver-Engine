// Tears and islands: resolveTears duplicates particles on the fans a break disconnected, planIslands
// orders the connected pieces, and the render binding follows. Pure arithmetic; no GPU, no Jolt.
#include "aver/core/Log.hpp"
#include "aver/softbody/Islands.hpp"
#include "aver/softbody/RenderBinding.hpp"

#include <cmath>
#include <string>
#include <vector>

using namespace aver;
namespace sb = aver::softbody;

static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

using Ids = std::vector<u32>;

// A cols x rows sheet in the XZ plane, spacing 10 cm. The left column is pinned when `pinLeft`.
static sb::Cage sheet(u32 cols, u32 rows, bool pinLeft) {
    sb::Cage c;
    sb::GridSpec g;
    g.cols = cols; g.rows = rows;
    g.du = Vec3{10, 0, 0}; g.dv = Vec3{0, 0, 10};
    sb::addGrid(c, g);
    if (pinLeft)
        for (u32 y = 0; y < rows; ++y) c.particles[y * cols].pinned = true;
    sb::build(c);
    sb::recomputeMasses(c);
    return c;
}

// Breaks every beam that joins a particle at or left of `xLeft` to one at or right of `xRight`.
static u32 cutBetween(sb::Cage& c, f32 xLeft, f32 xRight) {
    u32 n = 0;
    for (u32 bi = 0; bi < c.beams.size(); ++bi) {
        const f32 ax = c.particles[c.beams[bi].a0].rest.x, bx = c.particles[c.beams[bi].b0].rest.x;
        if ((ax <= xLeft && bx >= xRight) || (bx <= xLeft && ax >= xRight)) { sb::breakBeam(c, bi); ++n; }
    }
    return n;
}

static u32 findBeam(const sb::Cage& c, u32 a, u32 b) {
    for (u32 i = 0; i < c.beams.size(); ++i) {
        const sb::Beam& bm = c.beams[i];
        if ((bm.a0 == a && bm.b0 == b) || (bm.a0 == b && bm.b0 == a)) return i;
    }
    return sb::kNone;
}

int main() {
    AVER_INFO("IslandsTest");

    AVER_INFO("a strip cut across tears into exactly two islands");
    {
        sb::Cage c = sheet(6, 2, true);
        const sb::RenderBinding rb = sb::RenderBinding::identity(c);
        check(cutBetween(c, 20.0f, 30.0f) > 0, "the cut breaks the beams crossing the gap");
        check(sb::resolveTears(c) == 0, "no particle needs duplicating: every fan stays on one side");
        const sb::IslandPlan plan = sb::planIslands(c, 1);
        check(plan.islands.size() == 2 && plan.dust.empty(), "two islands, no dust");
        if (plan.islands.size() == 2) {
            check(plan.islands[0].anchored && !plan.islands[1].anchored, "the pinned side is first");
            check(plan.islands[0].particles == Ids({0, 1, 2, 6, 7, 8}), "the left island holds columns 0..2");
            check(plan.islands[1].particles == Ids({3, 4, 5, 9, 10, 11}), "the right island holds columns 3..5");
            check(plan.islands[0].triangles.size() == 4 && plan.islands[1].triangles.size() == 4, "four live triangles each");
            Ids side(c.particles.size(), 9);
            for (u32 i = 0; i < 2; ++i)
                for (const u32 p : plan.islands[i].particles) side[p] = i;
            bool whole = true;
            for (u32 ti = 0; ti < c.triangles.size(); ++ti) {
                if (c.triDead[ti]) continue;
                const sb::Triangle& t = c.triangles[ti];
                whole = whole && side[t.v[0]] == side[t.v[1]] && side[t.v[1]] == side[t.v[2]];
            }
            check(whole, "every live triangle lies wholly on one side");
        }
        check(sb::countPieces(c) == 2, "countPieces agrees");
        Ids visible;
        rb.visibleIndices(c.triDead, visible);
        check(visible.size() == 24, "the two triangles spanning the gap are left out of the draw (24 of 30 indices)");

        const sb::IslandPlan small = sb::planIslands(c, 100);
        check(small.islands.size() == 1 && small.islands[0].anchored && small.dust == Ids({3, 4, 5, 9, 10, 11}),
              "an unanchored piece under minPieceTriangles is dust; the anchored one stays");
    }

    AVER_INFO("an anchored island comes first even when it is lighter");
    {
        sb::Cage c = sheet(6, 2, true);
        cutBetween(c, 10.0f, 20.0f);
        sb::resolveTears(c);
        const sb::IslandPlan plan = sb::planIslands(c, 1);
        check(plan.islands.size() == 2, "two islands");
        if (plan.islands.size() == 2) {
            check(plan.islands[0].anchored && plan.islands[0].particles.size() == 4, "the 4-particle anchored piece is first");
            check(plan.islands[1].particles.size() == 8 && plan.islands[1].massKg > plan.islands[0].massKg, "the heavier free piece is second");
        }
    }

    AVER_INFO("unanchored pieces order by descending mass");
    {
        sb::Cage c = sheet(8, 2, false);
        cutBetween(c, 10.0f, 20.0f);   // 2 columns | 6 columns
        sb::resolveTears(c);
        const sb::IslandPlan plan = sb::planIslands(c, 1);
        check(plan.islands.size() == 2 && plan.islands[0].particles.size() == 12 && plan.islands[1].particles.size() == 4,
              "the heavier piece first");
    }

    AVER_INFO("a single broken edge in a sheet splits nothing");
    {
        sb::Cage c = sheet(5, 5, false);
        const u32 beam = findBeam(c, 2 * 5 + 2, 2 * 5 + 3);   // interior horizontal edge
        check(beam != sb::kNone, "found an interior edge");
        sb::breakBeam(c, beam);
        check(sb::resolveTears(c) == 0 && c.splitLog.empty(), "no particle is duplicated");
        const sb::IslandPlan plan = sb::planIslands(c, 1);
        check(plan.islands.size() == 1 && plan.dust.empty(), "one island");
        u32 dead = 0;
        for (const u8 d : c.triDead) dead += d;
        check(dead == 0, "no triangle is dead");
        check(sb::brokenBeamCount(c) == 1, "one broken beam");
    }

    AVER_INFO("a fan cut across duplicates the hub and the ring ends of the cut, and the render binding follows");
    {
        // A hexagonal fan: hub 0, ring 1..6, triangle i = (hub, ring i, ring i+1).
        sb::Cage c;
        const u32 hub = sb::addParticle(c, Vec3{0, 0, 0});
        u32 ring[6];
        for (u32 i = 0; i < 6; ++i) {
            const f32 a = static_cast<f32>(i) * kPi / 3.0f;
            ring[i] = sb::addParticle(c, Vec3{10.0f * std::cos(a), 0, 10.0f * std::sin(a)});
        }
        for (u32 i = 0; i < 6; ++i) sb::addTriangle(c, hub, ring[i], ring[(i + 1) % 6]);
        sb::build(c);
        sb::RenderBinding rb = sb::RenderBinding::identity(c);
        check(c.bends.size() == 6, "six interior spokes make six bends");

        sb::breakBeam(c, findBeam(c, hub, ring[0]));
        sb::breakBeam(c, findBeam(c, hub, ring[3]));
        check(sb::resolveTears(c) == 3, "the hub and the two ring ends of the cut are duplicated");
        // A ring vertex at the end of a broken spoke has only the two triangles either side of it, so it splits too.
        check(c.splitLog.size() == 3 && c.splitLog[0].oldParticle == hub && c.splitLog[0].newParticle == 7 &&
              c.splitLog[0].triangles == Ids({3, 4, 5}), "the hub's second fan moves to the duplicate");
        check(c.splitLog[1].oldParticle == ring[0] && c.splitLog[1].newParticle == 8 && c.splitLog[1].triangles == Ids({5}) &&
              c.splitLog[2].oldParticle == ring[3] && c.splitLog[2].newParticle == 9 && c.splitLog[2].triangles == Ids({3}),
              "each ring end hands one triangle to its duplicate");
        check(c.particles.size() == 10 && c.particles[7].origin == hub, "the duplicate remembers its origin");
        check(c.particles[7].pos.x == c.particles[hub].pos.x, "and starts where it was");
        bool moved = true;
        for (u32 ti = 3; ti < 6; ++ti) moved = moved && c.triangles[ti].v[0] == 7;
        for (u32 ti = 0; ti < 3; ++ti) moved = moved && c.triangles[ti].v[0] == hub;
        check(moved, "triangles 3..5 now use the duplicate");
        u32 dead = 0;
        for (const u8 d : c.triDead) dead += d;
        check(dead == 0, "no triangle spans a gap");

        Ids sources;
        rb.applySplits(c.splitLog, &sources);
        check(rb.vertexCount() == 10 && sources == Ids({0, ring[0], ring[3]}), "three render vertices are added, copied from their sources");
        check(rb.vertexParticle[7] == 7, "and follows the duplicate");
        bool follows = true;
        for (u32 t = 0; t < 3; ++t) follows = follows && rb.indices[t * 3] == 0;
        for (u32 t = 3; t < 6; ++t) follows = follows && rb.indices[t * 3] == 7;
        check(follows, "the moved triangles reference the new vertex");
        const sb::IslandPlan plan = sb::planIslands(c, 1);
        check(plan.islands.size() == 2 && sb::countPieces(c) == 2, "the fan comes apart into two pieces");
        check(sb::rebuildBends(c) == 4, "bends are rebuilt over the four unbroken spokes");
    }

    AVER_INFO("repair undoes a tear");
    {
        sb::Cage c = sheet(6, 2, true);
        const u32 bends = static_cast<u32>(c.bends.size());
        cutBetween(c, 20.0f, 30.0f);
        sb::resolveTears(c);
        check(sb::rebuildBends(c) < bends, "the cut removes the bends across it");
        sb::repair(c);
        check(sb::countPieces(c) == 1 && sb::brokenBeamCount(c) == 0 && c.particles.size() == c.baseParticleCount, "whole again");
        check(c.bends.size() == bends, "and the bends are back");
    }

    AVER_INFO("{} failure(s)", g_failures);
    return g_failures == 0 ? 0 : 1;
}
