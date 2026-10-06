// Crowd avoidance: the ORCA primitives, agents passing in a corridor, a head-on file in a narrow
// corridor, 200 agents through a doorway without overlap, determinism at the fixed step, and the
// backend / max-agents settings. Pure: a hand-drawn grid and a CrowdSim, no scene.
#include "SynapseAiTestUtil.hpp"

#include "aver/synapse/Crowd.hpp"
#include "aver/synapse/Steering.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

using namespace aver;
using namespace aver::synapse;
using aitest::check;

namespace {

bool approx(f32 a, f32 b, f32 eps = 1e-2f) { return std::fabs(a - b) <= eps; }

CrowdAgent makeAgent(u32 id, V2 pos, f32 radius, f32 maxSpeed) {
    CrowdAgent a;
    a.id = id;
    a.pos = pos;
    a.radius = radius;
    a.maxSpeed = maxSpeed;
    return a;
}

// ---- ORCA primitives ------------------------------------------------------------------------------

void testOrca() {
    AVER_INFO("ORCA: a lone agent keeps its preferred velocity, clipped to max speed");
    OrcaScratch scratch;
    {
        const std::vector<OrcaLine> none;
        const V2 v = orcaSolve(none, 0, {300, 0}, 100.0f, scratch);
        check(approx(v.x, 100.0f) && approx(v.y, 0.0f), "(300,0) at max 100 comes out (100,0)");
        const V2 slow = orcaSolve(none, 0, {40, 30}, 100.0f, scratch);
        check(approx(slow.x, 40.0f) && approx(slow.y, 30.0f), "a slower preference is untouched");
    }

    AVER_INFO("ORCA: a wall line caps the speed at which an agent may close on it");
    {
        // Wall point 40 cm away, agent radius 30: 10 cm of gap, 0.5 s horizon -> 20 cm/s allowed.
        std::vector<OrcaLine> lines{orcaWallLine({0, 0}, 30.0f, {40, 0}, {-1, 0}, 0.5f)};
        const V2 v = orcaSolve(lines, 1, {100, 0}, 100.0f, scratch);
        check(approx(v.x, 20.0f, 0.05f) && approx(v.y, 0.0f, 0.05f), "approach speed limited to gap / horizon");
        const V2 slide = orcaSolve(lines, 1, {100, 100}, 200.0f, scratch);
        check(approx(slide.x, 20.0f, 0.05f) && approx(slide.y, 100.0f, 0.05f), "motion along the wall is untouched");
        const V2 away = orcaSolve(lines, 1, {-50, 0}, 100.0f, scratch);
        check(approx(away.x, -50.0f) && approx(away.y, 0.0f), "moving away is always allowed");
    }

    AVER_INFO("ORCA: a wall the agent already overlaps pushes it out");
    {
        std::vector<OrcaLine> lines{orcaWallLine({0, 0}, 30.0f, {20, 0}, {-1, 0}, 0.5f)};
        const V2 v = orcaSolve(lines, 1, {100, 0}, 100.0f, scratch);
        check(v.x < 0.0f, "10 cm of penetration forces a velocity away from the wall");
    }

    AVER_INFO("ORCA: head-on agents get velocities that stay clear for the whole horizon");
    {
        const f32 r = 30.0f, horizon = 2.0f;
        const V2 pa{-200, 0}, pb{200, 0}, va{100, 0}, vb{-100, 0};
        std::vector<OrcaLine> la{orcaAgentLine(pa, va, r, pb, vb, r, 0.5f, horizon, 0.033f)};
        std::vector<OrcaLine> lb{orcaAgentLine(pb, vb, r, pa, va, r, 0.5f, horizon, 0.033f)};
        const V2 na = orcaSolve(la, 0, va, 100.0f, scratch);
        const V2 nb = orcaSolve(lb, 0, vb, 100.0f, scratch);
        f32 closest = 1e9f;
        for (int i = 0; i <= 200; ++i) {
            const f32 t = horizon * static_cast<f32>(i) / 200.0f;
            closest = std::min(closest, dist2(pa + na * t, pb + nb * t));
        }
        check(closest >= 2.0f * r - 0.5f, "the new velocities never bring them closer than touching");
        check(lenSq2(na - va) > 1.0f && lenSq2(nb - vb) > 1.0f, "both of them changed course (they share the effort)");
    }

    AVER_INFO("ORCA: a neighbour that cannot move is dodged by the one that can");
    {
        const V2 pa{0, 0}, pb{150, 0};
        std::vector<OrcaLine> la{orcaAgentLine(pa, {100, 0}, 30.0f, pb, {0, 0}, 30.0f, 1.0f, 2.0f, 0.033f)};
        const V2 v = orcaSolve(la, 0, {100, 0}, 100.0f, scratch);
        // Whatever it chose, extrapolated for the horizon it must not enter the static agent.
        f32 closest = 1e9f;
        for (int i = 0; i <= 200; ++i) closest = std::min(closest, dist2(pa + v * (2.0f * static_cast<f32>(i) / 200.0f), pb));
        check(closest >= 59.5f, "it steers around the immovable one");
    }
}

// ---- corridor -------------------------------------------------------------------------------------

fmt::OcNavData corridor() {
    // 30 x 5 cells of 50 cm: a corridor 150 cm wide (y 50..200) and 1500 cm long.
    return aitest::gridFrom({
        "##############################",
        "..............................",
        "..............................",
        "..............................",
        "##############################"});
}

void aimAtGoals(CrowdSim& sim, const std::vector<V2>& goals, f32 slow, f32 stop) {
    std::vector<CrowdAgent>& ag = sim.agentsMutable();
    for (usize i = 0; i < ag.size(); ++i)
        ag[i].prefVel = steerArrive(ag[i].pos, goals[ag[i].id], ag[i].maxSpeed, slow, stop);
}

void testCorridor() {
    AVER_INFO("corridor: two agents swap ends of a corridor, exactly head-on");
    const fmt::OcNavData nav = corridor();
    {
        CrowdSim sim;
        sim.setNav(&nav);
        check(sim.addAgent(makeAgent(0, {100, 125}, 30.0f, 200.0f)), "left agent added");
        check(sim.addAgent(makeAgent(1, {1400, 125}, 30.0f, 200.0f)), "right agent added");
        const std::vector<V2> goals{{1400, 125}, {100, 125}};

        f32 worst = 0.0f;
        for (int s = 0; s < 900; ++s) {   // 30 s
            aimAtGoals(sim, goals, 200.0f, 40.0f);
            sim.step();
            worst = std::max(worst, sim.worstOverlap());
        }
        const CrowdAgent* a = sim.find(0);
        const CrowdAgent* b = sim.find(1);
        check(a && a->pos.x > 1300.0f, "the left agent reached the right end");
        check(b && b->pos.x < 200.0f, "the right agent reached the left end");
        check(worst <= 6.0f, "they never overlapped by more than 6 cm (10% of the combined radius)");
        check(a && a->pos.y > 55.0f && a->pos.y < 195.0f && b && b->pos.y > 55.0f && b->pos.y < 195.0f,
              "and neither left the corridor");
    }

    AVER_INFO("corridor: four against four in a corridor two agents wide do not deadlock");
    {
        CrowdSim sim;
        sim.setNav(&nav);
        std::vector<V2> goals;
        for (u32 i = 0; i < 4; ++i) {
            sim.addAgent(makeAgent(i, {100.0f + 70.0f * static_cast<f32>(i), 125}, 30.0f, 200.0f));
            goals.push_back({1400, 125});
        }
        for (u32 i = 0; i < 4; ++i) {
            sim.addAgent(makeAgent(4 + i, {1400.0f - 70.0f * static_cast<f32>(i), 125}, 30.0f, 200.0f));
            goals.push_back({100, 125});
        }
        f32 worst = 0.0f;
        for (int s = 0; s < 1800; ++s) {   // 60 s
            aimAtGoals(sim, goals, 200.0f, 40.0f);
            sim.step();
            worst = std::max(worst, sim.worstOverlap());
        }
        u32 arrived = 0;
        for (const CrowdAgent& a : sim.agents())
            if (dist2(a.pos, goals[a.id]) < 120.0f) ++arrived;
        check(arrived == 8, "all eight reached their end (reached " + std::to_string(arrived) + ")");
        check(worst <= 9.0f, "without overlapping by more than 9 cm");
    }
}

// ---- doorway --------------------------------------------------------------------------------------

// Two rooms joined by a 200 cm door: 60 x 24 cells of 50 cm, wall column at x 1000..1050.
fmt::OcNavData doorwayGrid() {
    std::vector<std::string> rows;
    for (u32 y = 0; y < 24; ++y) {
        std::string row(60, '.');
        const bool door = y >= 10 && y <= 13;
        if (!door) row[20] = '#';
        rows.push_back(row);
    }
    return aitest::gridFrom(rows);
}

struct DoorwayResult {
    std::vector<V2> pos;
    f32 worstOverlap = 0.0f;
    u32 through = 0;
};

DoorwayResult runDoorway(const fmt::OcNavData& nav, u32 agents, u32 steps) {
    CrowdParams params;
    params.maxAgents = std::max(agents, 1u);
    params.contactIterations = 8;   // a dense funnel needs the push-apart to propagate further
    CrowdSim sim(params);
    sim.setNav(&nav);

    // Beyond the door everyone heads for the far (east) side, spread along it. They pile up there,
    // well clear of the door, so the door never backs up with agents that cannot get to a slot.
    std::vector<V2> goals;
    for (u32 k = 0; k < agents; ++k) {
        sim.addAgent(makeAgent(k, {80.0f + 60.0f * static_cast<f32>(k % 14), 100.0f + 70.0f * static_cast<f32>(k / 14)},
                               20.0f, 200.0f));
        goals.push_back({2700.0f, 100.0f + static_cast<f32>((k * 37u) % 1000u)});
    }

    DoorwayResult r;
    for (u32 s = 0; s < steps; ++s) {
        for (CrowdAgent& a : sim.agentsMutable()) {
            if (a.pos.x < 1075.0f) a.prefVel = steerSeek(a.pos, {1100.0f, 600.0f}, a.maxSpeed);
            else                   a.prefVel = steerArrive(a.pos, goals[a.id], a.maxSpeed, 150.0f, 15.0f);
        }
        sim.step();
        r.worstOverlap = std::max(r.worstOverlap, sim.worstOverlap());
    }
    for (const CrowdAgent& a : sim.agents()) {
        r.pos.push_back(a.pos);
        if (a.pos.x > 1100.0f) ++r.through;
    }
    return r;
}

void testDoorway() {
    AVER_INFO("doorway: 200 agents go through a 200 cm door without overlapping");
    const fmt::OcNavData nav = doorwayGrid();
    const DoorwayResult r = runDoorway(nav, 200, 3600);   // 120 s
    check(r.pos.size() == 200, "all 200 agents are simulated");
    check(r.worstOverlap <= 10.0f,
          "the worst overlap over the whole run is at most 10 cm (25% of the 40 cm combined radius): " +
              std::to_string(r.worstOverlap));
    check(r.through >= 150, "at least 150 are through the door after 120 s (got " + std::to_string(r.through) + ")");
    bool inside = true;
    for (const V2& p : r.pos) inside = inside && p.x > 0.0f && p.x < 3000.0f && p.y > 0.0f && p.y < 1200.0f;
    check(inside, "and none left the map");
}

void testDeterminism() {
    AVER_INFO("determinism: the same scene at the same fixed step gives bit-identical results");
    const fmt::OcNavData nav = doorwayGrid();
    const DoorwayResult a = runDoorway(nav, 60, 300);
    const DoorwayResult b = runDoorway(nav, 60, 300);
    check(a.pos.size() == b.pos.size() && !a.pos.empty(), "both runs simulated the same agents");
    check(std::memcmp(a.pos.data(), b.pos.data(), a.pos.size() * sizeof(V2)) == 0,
          "every position matches bit for bit");
    check(a.worstOverlap == b.worstOverlap, "and so does the worst overlap");
}

// ---- settings -------------------------------------------------------------------------------------

struct FakeBackend final : ICrowdBackend {
    bool avail = true;
    bool ready = true;
    V2 vel{10.0f, 0.0f};
    u32 solves = 0;
    const char* name() const override { return "fake"; }
    bool available() const override { return avail; }
    bool solve(const CrowdSolveInput& in, CrowdSolveOutput& out) override {
        ++solves;
        if (!ready) return false;
        out.ids.clear();
        out.vel.clear();
        for (u32 i = 0; i < in.count; ++i) { out.ids.push_back(in.agents[i].id); out.vel.push_back(vel); }
        return true;
    }
};

void testSettings() {
    AVER_INFO("settings: the max-agents cap and ids");
    {
        CrowdParams p;
        p.maxAgents = 3;
        CrowdSim sim(p);
        check(sim.addAgent(makeAgent(1, {0, 0}, 30, 100)) && sim.addAgent(makeAgent(2, {100, 0}, 30, 100)) &&
                  sim.addAgent(makeAgent(3, {200, 0}, 30, 100)),
              "three agents fit");
        check(!sim.addAgent(makeAgent(4, {300, 0}, 30, 100)), "the fourth is refused");
        check(!sim.addAgent(makeAgent(2, {400, 0}, 30, 100)), "a duplicate id is refused");
        check(sim.removeAgent(2) && sim.agents().size() == 2, "removal frees a slot");
        check(sim.addAgent(makeAgent(4, {300, 0}, 30, 100)), "which the fourth then takes");
        check(sim.find(4) && sim.find(1) && sim.find(3) && !sim.find(2), "ids still resolve after the removal");
        check(!sim.addAgent(makeAgent(9, {NAN, 0}, 30, 100)), "a non-finite position is refused");
    }

    AVER_INFO("settings: backend selection and fallback");
    {
        CrowdSim sim;
        sim.addAgent(makeAgent(0, {0, 0}, 30, 200));
        sim.agentsMutable()[0].prefVel = {100, 0};
        check(std::string(sim.activeBackendName()) == "cpu-orca", "CPU is the default");

        sim.setBackendKind(CrowdBackendKind::Gpu);
        check(std::string(sim.activeBackendName()) == "cpu-orca", "asking for GPU with none installed stays on CPU");

        FakeBackend fake;
        sim.setGpuBackend(&fake);
        check(std::string(sim.activeBackendName()) == "fake", "an available GPU backend is used");
        sim.step();
        check(fake.solves == 1, "the step went through it");
        check(approx(sim.agents()[0].vel.x, 10.0f, 0.01f), "and the agent followed its answer");

        fake.avail = false;
        check(std::string(sim.activeBackendName()) == "cpu-orca", "an unavailable one falls back to CPU");
        const u32 before = fake.solves;
        sim.step();
        check(fake.solves == before, "without being asked to solve");

        fake.avail = true;
        fake.ready = false;
        const V2 prev = sim.agents()[0].avoidVel;
        sim.step();
        check(sim.agents()[0].avoidVel.x == prev.x && sim.agents()[0].avoidVel.y == prev.y,
              "an answer that is not ready yet leaves the previous avoidance velocity in place");
    }

    AVER_INFO("settings: advance() runs whole fixed steps and caps catch-up");
    {
        CrowdSim sim;
        sim.addAgent(makeAgent(0, {0, 0}, 30, 200));
        const f32 dt = sim.params().fixedStep;
        check(sim.advance(dt * 0.5f) == 0, "half a step runs nothing");
        check(sim.advance(dt * 0.5f + 1e-4f) == 1, "the other half completes one");
        check(sim.advance(dt * 100.0f) == sim.params().maxStepsPerAdvance, "a long stall is capped");
    }
}

} // namespace

int main() {
    AVER_INFO("CrowdTest");
    testOrca();
    testCorridor();
    testDoorway();
    testDeterminism();
    testSettings();
    return aitest::g_failures == 0 ? 0 : 1;
}
