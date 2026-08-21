#pragma once
// The join between Synapse's pure pathfinder and a live scene: CSynapseAgent, and the system that
// walks a registered agent's baked path toward its goal, one frame at a time.
//
// A SECOND TARGET, deliberately, for the reason Aver.Anim.Scene is one (see that module's own
// CMakeLists comment). This header sits at aver/synapse/, not aver/synapse/scene/, and the
// namespace below is plain aver::synapse -- matching Aver.Anim.Scene's own AnimSystem.hpp, which
// lives at aver/anim/ despite its module directory being modules/anim.scene. A namespace segment
// literally named "scene" nested inside aver::synapse would shadow the SIBLING namespace
// aver::scene for every reference in this file, turning every bare `scene::X` into a lookup for a
// nonexistent aver::synapse::scene::X -- caught by the compiler, not worth re-inviting by matching
// the directory name instead of the precedent.
//
// Aver.Synapse is pure grid-and-arithmetic so it can be tested against a hand-authored grid with no
// world at all, and folding scene::World in here would end that. This module is the "somebody"
// that resolves a goal into a scene::Entity's actual path.
//
// SYNAPSE ADVISES, IT DOES NOT MOVE. This system tracks a path and exposes the agent's CURRENT
// waypoint through CSynapseAgent's own fields -- it never calls setLocalPosition or otherwise
// touches the entity's transform. Turning "where should I be heading" into an actual capsule
// velocity is SynapseSteer + the existing CharacterMove node's job, wired explicitly by whoever
// authors the agent's graph -- see the design doc's own "Steering" section for why that split is
// not optional (AverCharacter's capsule handle is private; nothing outside Aver.Framework can drive
// it directly).
#include "aver/core/Math.hpp"
#include "aver/core/Types.hpp"
#include "aver/formats/OcNav.hpp"
#include "aver/scene/World.hpp"

#include <unordered_map>
#include <vector>

namespace aver::synapse {

enum class AgentStatus : i32 {
    None      = 0,   // no goal has ever been set -- the system leaves this agent alone
    Requested = 1,   // a goal was just set; the system has not attempted to path to it yet
    Pathing   = 2,   // a path exists and the agent has not reached its last waypoint
    Arrived   = 3,   // the last waypoint was reached
    Failed    = 4,   // the goal was Unreachable, OffMesh, or the grid was Invalid -- given up, not retried
};

// Registered via World::registerComponent, so it carries no waypoint list of its own -- an array
// field has no FieldKind (Fields.hpp enumerates scalars, Vec3/Quat/Mat4 and String, nothing
// variable-length), and the reflection API's own contract is "verify covers the struct byte for
// byte". The path itself lives in AgentSystem's own side table, exactly as AnimSystem keeps a
// posed entity's skinning matrices OUT of CAnimator for the identical reason.
struct CSynapseAgent {
    // Physical size. Defaults MATCH AverCharacter's own (Character.cs:62,65; also restated in
    // OcNavData's own bake-parameter comment) -- these are not invented numbers, and a mismatch
    // would mean this agent's capsule does not actually fit where the grid says it can stand.
    f32 radiusCm = 34.0f;
    f32 heightCm = 180.0f;

    // Tuning. moveSpeedCm matches AverCharacter.MoveSpeed's own default (Character.cs:47) --
    // informational for now: CharacterMove reads AverCharacter.MoveSpeed directly, not this field,
    // so nothing downstream consumes it yet. turnRateDegPerSec and arriveRadiusCm have no existing
    // engine analogue to match, so these values are this module's own choice: a full about-face in
    // one second, and an arrival tolerance a little over the capsule's own diameter.
    f32 moveSpeedCm         = 350.0f;
    f32 turnRateDegPerSec   = 180.0f;
    f32 arriveRadiusCm      = 50.0f;

    // The goal, and where the system's own pathing has gotten to.
    f32 goalXCm = 0.0f, goalYCm = 0.0f, goalZCm = 0.0f;
    i32 status  = 0;   // an AgentStatus

    // The agent's CURRENT steering target -- the next waypoint on its path, or the goal itself once
    // there is only one leg left. Valid (and only meaningful) while status == Pathing. This is the
    // one thing a graph is meant to read; see GetSynapseTarget (Aver Node) and the design doc's
    // "Steering" section for why it is a target position and not a precomputed forward/right/yaw.
    f32 targetXCm = 0.0f, targetYCm = 0.0f, targetZCm = 0.0f;
};

// Owns the registered component id and, for each Pathing agent, its current baked path.
class AgentSystem {
public:
    // Registers CSynapseAgent, idempotently -- safe to call from more than one composition root
    // path, or more than once. Call before spawning anything that will carry the component.
    u32 registerComponents(aver::scene::World& world);
    // 0 until registerComponents() has run.
    u32 componentType() const { return type_; }

    // Adds CSynapseAgent to `e` with its REAL defaults applied. world.addComponent alone is not
    // enough: ComponentPool::add resizes its byte buffer with a raw zero fill (ComponentPool.cpp) --
    // it has no way to run a constructor over type-erased storage, so a caller that attached the
    // component directly would get moveSpeedCm == 0 and an agent that never moves. World::create()
    // solves the identical problem for CLocal/CWorld/CName/CHierarchy the same way (assigning a
    // fresh default-constructed value over the zeroed bytes rather than trusting the type's own
    // in-class defaults to have applied themselves) -- see World.cpp's own create(). nullptr if
    // registerComponents() has not run or `e` is not live.
    CSynapseAgent* attach(aver::scene::World& world, aver::scene::Entity e);

    // Points an existing CSynapseAgent at a new goal, discarding whatever path it had. False when
    // `e` does not carry CSynapseAgent (attach it first -- this system does not add it implicitly,
    // the same way AnimSystem never adds a CAnimator) or registerComponents() has not run.
    bool setGoal(aver::scene::World& world, aver::scene::Entity e, const aver::Vec3& goalCm);

    // Advances every CSynapseAgent by one tick:
    //  - Requested agents attempt exactly one findPath call, IF a valid nav grid is available this
    //    tick. No grid yet is not a failure -- the attempt is simply deferred to a later tick (this
    //    is what lets a level whose .ocnav loads a frame late, per SandboxApp's own async loader,
    //    still path correctly once it arrives).
    //  - Pathing agents advance past any waypoint already within arriveRadiusCm of their CURRENT
    //    position (read from the entity's own world matrix -- this system never assumes CLocal is
    //    world space), and Arrive once the last one is behind them.
    //  - Unreachable/OffMesh/Invalid results become Failed EXACTLY ONCE and are never retried --
    //    the "gives up rather than spinning" half of this system's contract. A caller that wants
    //    another attempt calls setGoal() again, even with the same goal.
    void tick(aver::scene::World& world, const aver::fmt::OcNavData* nav);

    // How many agents currently hold a live path. Exists so a test can assert the side table does
    // not leak once an agent Arrives, Fails, or is destroyed, without reaching into private state.
    u32 activePathCount() const { return static_cast<u32>(paths_.size()); }

private:
    struct AgentPath {
        std::vector<Vec3> points;
        u32 index = 0;
    };

    u32 type_ = 0;
    // KEYED BY THE FULL ENTITY HANDLE, pruned every tick -- AnimSystem::posed_'s own documented
    // fix, applied here for the identical reason: an entity index is recycled, its generation is
    // not, and a stale index-only key would hand a fresh spawn a stranger's half-finished path.
    std::unordered_map<aver::scene::Entity, AgentPath> paths_;

    void prune(aver::scene::World& world);
};

// The process-global system, matching anim::animSystem() / scene::World::instance().
AgentSystem& agentSystem();

} // namespace aver::synapse
