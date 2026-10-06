#pragma once
// SynapseAi: the crowd, hearing and tactics systems under one roof, plus the behaviour-tree
// actions and conditions that drive them.
//
// This class is NOT a process-global. The singleton lives in Aver.Synapse.Abi (one copy, behind
// the C ABI), so a composition root that links the static libraries too never ends up with a
// second, silent instance. Tests and tools construct their own.
//
// Frame order: AgentSystem::tick, then SynapseAi::tick, then BtSystem::tick.
#include "aver/synapse/Bt.hpp"
#include "aver/synapse/SynapseCrowd.hpp"
#include "aver/synapse/SynapseHearing.hpp"
#include "aver/synapse/SynapseTactics.hpp"

namespace aver::synapse {

class SynapseAi {
public:
    SynapseAi();

    // Registers every component of the three systems. Idempotent.
    void registerComponents(scene::World& world);

    // Hearing, then tactics (cover, squads), then crowd.
    void tick(scene::World& world, const fmt::OcNavData* nav, f32 dt);

    // Installs the behaviour-tree vocabulary below into `reg`, bound to this object:
    //   Conditions
    //     HeardNoise            hearing memory confidence > params[0]
    //     HeardNoiseTag         a memory with tag params[0]
    //     InCover               holds a reserved point and stands on it (params[0] = reach, cm)
    //     SquadRoleIs           CSynapseSquad.role == params[0] (SquadRole)
    //   Actions
    //     InvestigateNoise      path to the strongest remembered noise; Running until there
    //     TakeCover             params: [0] threat 0 = seen target, 1 = heard noise; [1] max seek cm;
    //                           [2] min threat distance cm; [3] 1 = full-height only.
    //                           Claims a point, paths to it, Running until there
    //     LeaveCover            releases the reservation
    //     SquadFlank            path to this member's squad slot; Running until there
    //     SquadKeepSpacing      steps out of squad-mates' spacing circles; Success when clear
    //     SquadSetTarget        params[0..2] = target; applies to this member's squad
    //     CrowdSetMode          params[0] = CrowdMode, [1..3] = steer target
    void registerBehaviors(BtRegistry& reg);

    void reset() { crowd_.reset(); hearing_.reset(); tactics_.reset(); }

    CrowdSystem& crowd() { return crowd_; }
    HearingSystem& hearing() { return hearing_; }
    TacticsSystem& tactics() { return tactics_; }

    // Cached component type lookups for the behaviours (0 until registered).
    u32 agentType(scene::World& w);
    u32 perceptionType(scene::World& w);

private:
    CrowdSystem crowd_;
    HearingSystem hearing_;
    TacticsSystem tactics_;
    u32 agentType_ = 0;
    u32 perceptionType_ = 0;
};

} // namespace aver::synapse
