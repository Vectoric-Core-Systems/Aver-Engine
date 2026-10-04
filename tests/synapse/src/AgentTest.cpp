// CSynapseAgent + AgentSystem::tick -- a goal turns into a path, a path turns into a moving
// target, and an impossible goal is given up on rather than retried every frame.
//
// SYNAPSE ADVISES, IT DOES NOT MOVE (see SynapseAgent.hpp's own header comment) -- AgentSystem
// never touches an entity's transform. So this test drives its OWN tiny reference "steering" step
// each tick (move toward CSynapseAgent's current target at moveSpeedCm, capped so it cannot
// overshoot) to prove the whole loop converges end to end, standing in for what the real
// SynapseSteer node + CharacterMove would do from a graph. That stand-in is deliberately dumber
// than SynapseSteer (no turning, no facing) -- it exists to move the entity, not to re-implement
// steering, and the real node has its own coverage at the GraphCompiler level.
#include "aver/synapse/SynapseAgent.hpp"

#include "aver/core/Log.hpp"
#include "aver/scene/World.hpp"
#include "aver/synapse/Nav.hpp"

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

// Destroys every entity, so each block starts from a world it fully owns -- SaveWorldTest.cpp's
// own helper, copied rather than shared across test executables that link different modules.
static void clearWorld(scene::World& w) {
    std::vector<scene::Entity> all;
    for (u32 i = 0; i < w.count(); ++i) all.push_back(w.at(i));
    for (const scene::Entity e : all) if (w.valid(e) && !w.destroyPending(e)) w.destroy(e);
    w.flush();
}

// Builds a grid from ASCII rows, '.' walkable and '#' not -- NavTest.cpp's own helper, copied for
// the same reason clearWorld is: this executable links Aver.Scene, NavTest's does not, and neither
// links the other's test sources.
static fmt::OcNavData gridFrom(const std::vector<std::string>& rows, f32 cell = 50.0f) {
    fmt::OcNavData nav;
    nav.cellSizeCm = cell;
    nav.heightCells = static_cast<u32>(rows.size());
    nav.widthCells = rows.empty() ? 0 : static_cast<u32>(rows[0].size());
    nav.cells.resize(static_cast<usize>(nav.widthCells) * nav.heightCells);
    for (u32 y = 0; y < nav.heightCells; ++y) {
        for (u32 x = 0; x < nav.widthCells; ++x) {
            fmt::OcNavCell& c = nav.cells[static_cast<usize>(y) * nav.widthCells + x];
            if (rows[y][x] == '#') { c.flags = 0; continue; }
            c.flags = fmt::kOcNavWalkable;
        }
    }
    synapse::buildRegions(nav);
    return nav;
}

static Vec3 cellCentre(const fmt::OcNavData& nav, u32 x, u32 y) {
    return synapse::cellToWorld(nav, x, y);
}

int main() {
    AVER_INFO("AgentTest");
    scene::World& w = scene::World::instance();

    const u32 type = synapse::agentSystem().registerComponents(w);
    check(type != 0, "CSynapseAgent registers");

    AVER_INFO("an agent given a reachable goal reaches it in a headless tick loop");
    {
        clearWorld(w);

        //  y=4  . . . . .
        //  y=3  . . # . .
        //  y=2  . . # . .
        //  y=1  . . # . .
        //  y=0  . . . . .     <- the gap along the bottom; the same bent-path grid NavTest.cpp uses
        const fmt::OcNavData nav = gridFrom({
            ".....",
            "..#..",
            "..#..",
            "..#..",
            "....."});

        const Vec3 start = cellCentre(nav, 0, 3);
        const Vec3 goal  = cellCentre(nav, 4, 3);

        Transform xf; xf.position = start;
        const scene::Entity e = w.create("Agent", scene::kInvalidEntity, xf);
        check(synapse::agentSystem().attach(w, e) != nullptr, "the agent takes CSynapseAgent");
        check(synapse::agentSystem().setGoal(w, e, goal), "setGoal accepts a live agent");

        auto* a = w.component<synapse::CSynapseAgent>(e, type);
        check(a != nullptr, "and the component reads back");
        if (a) check(a->status == static_cast<i32>(synapse::AgentStatus::Requested),
                     "status is Requested the instant a goal is set, before any tick has run");

        // The reference driver: dumb "walk toward the current target" steering, standing in for
        // SynapseSteer + CharacterMove. dt is a plain fixed step; nothing here is a frame-rate test.
        const f32 dt = 1.0f / 60.0f;
        const u32 kTickCap = 1000;   // generous: a 250x250cm grid at 350cm/s arrives in well under 100
        u32 ticks = 0;
        for (; ticks < kTickCap; ++ticks) {
            synapse::agentSystem().tick(w, &nav);
            a = w.component<synapse::CSynapseAgent>(e, type);
            if (!a || a->status == static_cast<i32>(synapse::AgentStatus::Arrived)) break;
            check(a->status != static_cast<i32>(synapse::AgentStatus::Failed),
                  "a reachable goal never reports Failed");
            if (a->status != static_cast<i32>(synapse::AgentStatus::Pathing)) continue;

            const Transform& cur = w.localTransform(e);
            const Vec3 target{a->targetXCm, a->targetYCm, a->targetZCm};
            Vec3 toTarget = target - cur.position;
            const f32 dist = std::sqrt(toTarget.x * toTarget.x + toTarget.y * toTarget.y);
            const f32 step = a->moveSpeedCm * dt;
            Vec3 next = cur.position;
            if (dist <= step || dist < 1e-4f) {
                next = target;
            } else {
                next.x += toTarget.x / dist * step;
                next.y += toTarget.y / dist * step;
                next.z = target.z;
            }
            w.setLocalPosition(e, next);
        }

        check(ticks < kTickCap, "it arrived within the tick cap, not by exhausting it");
        a = w.component<synapse::CSynapseAgent>(e, type);
        check(a && a->status == static_cast<i32>(synapse::AgentStatus::Arrived),
              "and its FINAL status is Arrived");

        const Transform& final_ = w.localTransform(e);
        const f32 dx = final_.position.x - goal.x, dy = final_.position.y - goal.y;
        const f32 distToGoal = std::sqrt(dx * dx + dy * dy);
        check(a && distToGoal <= a->arriveRadiusCm + 1e-3f,
              "and it is standing within arriveRadiusCm of the ACTUAL goal, not just 'some waypoint'");
        check(synapse::agentSystem().activePathCount() == 0,
              "AND THE SIDE TABLE IS EMPTY -- Arrived must not leak a finished path forever");
    }

    AVER_INFO("an agent given an unreachable goal gives up rather than spinning");
    {
        clearWorld(w);

        //  A room walled off completely from the rest of the grid -- NavTest.cpp's own sealed-room
        //  shape, reused so the O(1) Unreachable refusal is exercised the identical way.
        const fmt::OcNavData nav = gridFrom({
            ".......",
            ".......",
            "..###..",
            "..#.#..",
            "..###..",
            ".......",
            "......."});

        const Vec3 start = cellCentre(nav, 0, 0);
        const Vec3 goal  = cellCentre(nav, 3, 3);   // inside the sealed room

        Transform xf; xf.position = start;
        const scene::Entity e = w.create("StuckAgent", scene::kInvalidEntity, xf);
        synapse::agentSystem().attach(w, e);
        synapse::agentSystem().setGoal(w, e, goal);

        synapse::agentSystem().tick(w, &nav);
        auto* a = w.component<synapse::CSynapseAgent>(e, type);
        check(a && a->status == static_cast<i32>(synapse::AgentStatus::Failed),
              "ONE tick is enough to know the goal is unreachable -- the region check is O(1)");
        check(synapse::agentSystem().activePathCount() == 0,
              "and no path was ever recorded for it");

        for (int i = 0; i < 200; ++i) synapse::agentSystem().tick(w, &nav);

        a = w.component<synapse::CSynapseAgent>(e, type);
        check(a && a->status == static_cast<i32>(synapse::AgentStatus::Failed),
              "AND IT STAYS FAILED -- 200 more ticks do not re-attempt the same impossible goal, "
              "which is the whole difference between giving up and spinning");
        check(synapse::agentSystem().activePathCount() == 0,
              "still no path recorded after 200 ticks -- nothing was ever attempted again");
    }

    AVER_INFO(g_failures ? "AgentTest: {} FAILURES" : "AgentTest: all checks passed ({})", g_failures);
    return g_failures ? 1 : 0;
}
