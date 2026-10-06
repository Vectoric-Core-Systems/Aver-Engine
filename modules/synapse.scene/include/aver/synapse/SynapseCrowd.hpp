#pragma once
// The join between the pure crowd simulation (Aver.Synapse's CrowdSim) and a live scene::World.
//
// CSynapseCrowd marks an entity as a crowd agent. Its preferred velocity comes from a steering
// mode: follow the CSynapseAgent path target (the default), or seek/arrive/flee/wander/hold. The
// avoidance solver turns that into a collision-free velocity, published on the component.
//
// Two drive modes:
//  - Advise (default): like the rest of Synapse the system does not move anything. A graph reads
//    velXCm/velYCm and feeds a character controller. The entity's own position is the truth.
//  - Move: the crowd is the truth and writes the entity's local position each tick. For large
//    crowds that have no physics capsule (and the only mode that makes sense for thousands).
//
// Tick order in a frame: AgentSystem::tick (waypoints), then CrowdSystem::tick, then the BT tick.
#include "aver/formats/OcNav.hpp"
#include "aver/scene/World.hpp"
#include "aver/synapse/Crowd.hpp"
#include "aver/synapse/Steering.hpp"

#include <unordered_map>
#include <vector>

namespace aver::synapse {

enum class CrowdMode : i32 {
    FollowAgent = 0,   // head for CSynapseAgent's current target while it is Pathing
    Seek   = 1,        // steer fields = target
    Arrive = 2,
    Flee   = 3,        // steer fields = threat
    Wander = 4,
    Hold   = 5,        // stand still; others avoid it
};

enum class CrowdDrive : u8 { Advise = 0, Move = 1 };

// All 4-byte scalars so the reflection table can cover the struct byte for byte.
struct CSynapseCrowd {
    f32 radiusCm   = 34.0f;
    f32 maxSpeedCm = 350.0f;
    f32 maxAccelCm = 1200.0f;
    f32 priority   = 0.0f;
    i32 mode       = 0;          // a CrowdMode
    f32 steerXCm = 0.0f, steerYCm = 0.0f, steerZCm = 0.0f;
    f32 arriveSlowRadiusCm = 300.0f;
    f32 arriveStopRadiusCm = 40.0f;
    f32 fleePanicRadiusCm  = 800.0f;
    i32 enabled = 1;
    // Outputs, written by the tick.
    i32 active = 0;              // 1 while simulated; 0 when disabled or past the max-agents cap
    f32 velXCm = 0.0f, velYCm = 0.0f, speedCm = 0.0f;
    f32 desiredXCm = 0.0f, desiredYCm = 0.0f;
    i32 stuck = 0;               // 1 while in a stand-off
};

class CrowdSystem {
public:
    CrowdSystem();

    u32 registerComponents(scene::World& world);
    u32 componentType() const { return type_; }
    // Adds CSynapseCrowd with real defaults, taking radius and speed from CSynapseAgent when the
    // entity carries one.
    CSynapseCrowd* attach(scene::World& world, scene::Entity e);

    // Cpu is always available. Gpu needs `gpu` (an Aver.Synapse.Gpu backend, see CrowdGpu.hpp)
    // and falls back to Cpu while it is null or unavailable.
    void setBackend(CrowdBackendKind kind, ICrowdBackend* gpu = nullptr);
    CrowdBackendKind backendKind() const { return sim_.backendKind(); }
    const char* activeBackendName() const { return sim_.activeBackendName(); }

    // Agents beyond this (lowest entity ids kept) are left unsimulated and report active = 0.
    void setMaxAgents(u32 n);
    u32 maxAgents() const { return sim_.params().maxAgents; }

    void setDrive(CrowdDrive d) { drive_ = d; }
    CrowdDrive drive() const { return drive_; }

    CrowdSim& sim() { return sim_; }
    const CrowdSim& sim() const { return sim_; }

    void tick(scene::World& world, const fmt::OcNavData* nav, f32 dt);

    // Drops every simulated agent (settings stay).
    void reset() { sim_.clear(); wander_.clear(); overflow_ = 0; }

    u32 simulatedCount() const { return static_cast<u32>(sim_.agents().size()); }
    u32 overflowCount() const { return overflow_; }

private:
    u32 type_ = 0;
    CrowdDrive drive_ = CrowdDrive::Advise;
    CrowdSim sim_;
    u32 overflow_ = 0;
    std::unordered_map<scene::Entity, WanderState> wander_;
    std::vector<scene::Entity> kept_;
    std::vector<u32> drop_;
};

} // namespace aver::synapse
