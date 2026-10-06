#pragma once
// Crowd simulation on the ground plane: many agents steering toward their own preferred velocity
// while avoiding each other and the baked navigation grid's walls.
//
// Two pieces:
//  - CrowdSim: owns the agents, steps at a FIXED step, and finishes every step with a
//    position-based contact pass so agents never end a step overlapping each other or a wall,
//    even in a dense doorway where velocity avoidance alone cannot keep everyone clear.
//  - A pluggable velocity solver (ICrowdBackend). The CPU solver (ORCA over a uniform grid) is the
//    default and is deterministic. A GPU solver (Aver.Synapse.Gpu) can replace it for thousands of
//    agents; it lives in another module so this one stays free of the RHI.
//
// PURE: no scene, no physics. Aver.Synapse.Scene's CrowdSystem is the join to entities.
#include "aver/formats/OcNav.hpp"
#include "aver/synapse/CrowdOrca.hpp"

#include <unordered_map>
#include <vector>

namespace aver::synapse {

enum class CrowdBackendKind : u8 { Cpu = 0, Gpu = 1 };

inline constexpr u32 kCrowdStatic = 1u << 0;   // CrowdAgent::flags: never moves, others avoid it

struct CrowdAgent {
    u32 id = 0;                 // caller's key; unique per sim
    V2  pos, vel;
    V2  prefVel;                // where the agent wants to go (cm/s); set by steering each tick
    f32 radius   = 34.0f;
    f32 maxSpeed = 350.0f;
    f32 maxAccel = 1200.0f;     // cm/s^2
    f32 priority = 0.0f;        // higher wins a stand-off; ties go to the lower id
    u32 flags    = 0;
    // Owned by the sim.
    V2  avoidVel;               // the solver's latest answer
    f32 stuckSec = 0.0f;
};

struct CrowdParams {
    f32 fixedStep        = 1.0f / 30.0f;
    f32 timeHorizon      = 1.5f;     // seconds ahead agents avoid each other
    f32 obstacleHorizon  = 0.6f;     // seconds ahead agents avoid walls
    f32 neighborRadiusCm = 500.0f;
    u32 maxNeighbors     = 10;
    u32 maxWallLines     = 6;
    u32 contactIterations = 4;
    // Stand-off handling: an agent moving slower than stuckSpeedFrac of its preferred speed for
    // stuckSeconds, facing another such agent, lets the lower-priority one yield.
    f32 stuckSpeedFrac   = 0.3f;
    f32 stuckSeconds     = 0.5f;
    f32 sidestepBias     = 0.03f;    // fraction of preferred speed steered to the right, breaks symmetry
    u32 maxAgents        = 1024;     // addAgent refuses past this
    u32 maxStepsPerAdvance = 4;      // a long frame never runs more steps than this
};

// Uniform grid over agent positions, in CSR form (also what the GPU backend uploads).
struct CrowdGrid {
    static constexpr u64 kMaxCells = 262144;
    f32 cellSize = 400.0f;
    f32 minX = 0.0f, minY = 0.0f;
    u32 w = 0, h = 0;
    std::vector<u32> cellStart;   // w*h + 1
    std::vector<u32> items;       // agent indices; ascending inside each cell

    // cellSize is at least minCell, and grows if the extent would need more than kMaxCells cells.
    void build(const CrowdAgent* agents, u32 count, f32 minCell);
    void cellOf(V2 p, i32& cx, i32& cy) const;
};

struct CrowdSolveInput {
    const CrowdAgent*      agents = nullptr;
    u32                    count  = 0;
    const CrowdParams*     params = nullptr;
    const fmt::OcNavData*  nav    = nullptr;   // walls; may be null
};

struct CrowdSolveOutput {
    std::vector<u32> ids;   // aligned with vel; the agents that were solved
    std::vector<V2>  vel;
};

class ICrowdBackend {
public:
    virtual ~ICrowdBackend() = default;
    virtual const char* name() const = 0;
    // False when the backend cannot run (no device); the sim then uses the CPU solver.
    virtual bool available() const = 0;
    // Fills `out` and returns true when a result is ready. An asynchronous backend may return
    // false while its answer is in flight; the sim keeps each agent's previous avoidVel.
    virtual bool solve(const CrowdSolveInput& in, CrowdSolveOutput& out) = 0;
};

// ORCA over a uniform grid. Deterministic: agents are visited in index order, neighbours in
// (distance, index) order, and nothing reads another agent's new velocity.
class CrowdCpuSolver final : public ICrowdBackend {
public:
    const char* name() const override { return "cpu-orca"; }
    bool available() const override { return true; }
    bool solve(const CrowdSolveInput& in, CrowdSolveOutput& out) override;

    // The preferred velocity actually fed to the solver: prefVel plus the small sidestep bias.
    static V2 biasedPreferred(const CrowdAgent& a, const CrowdParams& p);

private:
    CrowdGrid grid_;
    std::vector<OrcaLine> lines_;
    std::vector<std::pair<f32, u32>> cands_;
    OrcaScratch scratch_;
};

class CrowdSim {
public:
    explicit CrowdSim(const CrowdParams& p = {}) : params_(p) {}

    void setParams(const CrowdParams& p) { params_ = p; }
    const CrowdParams& params() const { return params_; }

    // The grid whose unwalkable cells are walls. The sim does not own it; keep it alive.
    void setNav(const fmt::OcNavData* nav) { nav_ = nav; }

    // Cpu always works. Gpu falls back to Cpu while no available backend is set.
    void setBackendKind(CrowdBackendKind k) { kind_ = k; }
    CrowdBackendKind backendKind() const { return kind_; }
    void setGpuBackend(ICrowdBackend* b) { gpu_ = b; }
    // The solver the NEXT step will use ("cpu-orca", or the GPU backend's name).
    const char* activeBackendName() const;

    // False when full or the id exists. The agent's avoidVel starts at its clamped prefVel.
    bool addAgent(const CrowdAgent& a);
    bool removeAgent(u32 id);
    CrowdAgent* find(u32 id);
    const CrowdAgent* find(u32 id) const;
    void clear();
    const std::vector<CrowdAgent>& agents() const { return agents_; }
    std::vector<CrowdAgent>& agentsMutable() { return agents_; }

    // One fixed step.
    void step();
    // Accumulates dt and runs whole fixed steps (at most maxStepsPerAdvance). Returns how many ran.
    u32 advance(f32 dt);

    // Largest overlap (cm) between any two agents, or any agent and a wall, right now. For tests
    // and debugging; O(n) with the grid.
    f32 worstOverlap() const;

private:
    CrowdParams params_;
    const fmt::OcNavData* nav_ = nullptr;
    CrowdBackendKind kind_ = CrowdBackendKind::Cpu;
    ICrowdBackend* gpu_ = nullptr;
    CrowdCpuSolver cpu_;
    CrowdSolveOutput out_;
    CrowdGrid grid_;
    std::vector<CrowdAgent> agents_;
    std::unordered_map<u32, u32> index_;
    f32 accumulator_ = 0.0f;

    void reindex();
    void resolveContacts();
    void pushOutOfWalls(CrowdAgent& a) const;
};

} // namespace aver::synapse
