#pragma once
// The join between the pure BT evaluator (Aver.Synapse) and a live scene::World: CSynapseBehavior,
// BtSystem's per-entity tick, and the seven built-in Condition/Action names every project gets for
// free (registerBuiltinBehaviors) on top of Aver.Synapse's own BtRegistry, which a project may
// still add its own named entries to afterward.
//
// Plain aver::synapse namespace, NOT aver::synapse::scene -- see SynapseAgent.hpp's own header
// comment for why a namespace segment literally named "scene" would shadow the sibling namespace
// aver::scene for every bare `scene::X` reference in this file.
#include "aver/core/Math.hpp"
#include "aver/core/Types.hpp"
#include "aver/formats/OcBt.hpp"
#include "aver/scene/World.hpp"
#include "aver/synapse/Bt.hpp"
#include "aver/synapse/SynapsePerception.hpp"   // reuses its NotifyFn -- see setNotifySink's own comment

#include <string>
#include <unordered_map>

namespace aver::synapse {

// Registered via World::registerComponent, all scalars -- the RUNNING state (which composite node
// is mid-Sequence, Cooldown timers, an Action's own scratch float) is NOT part of this component,
// for the identical reason CSynapseAgent keeps its own path out of itself: BtRunningState holds
// std::unordered_maps, which the field-table reflection API (Fields.hpp) has no FieldKind for. It
// lives in BtSystem's own side table instead, keyed by the full entity handle and pruned every
// tick -- see AgentSystem::paths_'s own comment for why "full handle, pruned" and not "index, kept
// forever" is the only safe choice.
struct CSynapseBehavior {
    u64 treeAssetId = 0;   // 0 = no tree assigned; BtSystem::loadTree returns the id to store here
    i32 lastStatus  = 0;   // a BtStatus, from the most recent tick -- readable for debugging/queries
};

class BtSystem {
public:
    // Registers CSynapseBehavior, idempotently. Call before spawning anything that will carry it.
    u32 registerComponents(scene::World& world);
    u32 componentType() const { return type_; }

    // Adds CSynapseBehavior to `e` with its REAL defaults applied -- see AgentSystem::attach's own
    // comment (SynapseAgent.hpp) for why a bare world.addComponent alone is not enough.
    CSynapseBehavior* attach(scene::World& world, scene::Entity e);

    // Loads and caches a .ocbt by path (keyed by fnv1a64 of the path, the same identity scheme
    // .ocworld placements already use). Returns the id to store on
    // CSynapseBehavior::treeAssetId, or 0 on a failed load (logged once, here, at the WARN level --
    // not cached as a permanent failure, so a path that starts resolving later -- content still
    // syncing, say -- is retried on the next attach rather than staying broken for the process
    // lifetime). Safe to call more than once for the same path; the second call returns the
    // already-cached id without touching disk again.
    u64 loadTree(const std::string& path);

    // Installed onto every BtSystem-owned "FireEvent" action -- see NotifyFn's own comment
    // (SynapsePerception.hpp) for why this is the SAME function signature a composition root
    // already installs onto anim::animSystem() and synapse::perceptionSystem(), and why installing
    // the identical function here too (rather than inventing a fourth copy) is correct, not a
    // shortcut: its body was never anim- or perception-specific.
    void setNotifySink(NotifyFn fn, void* user) { notify_ = fn; notifyUser_ = user; }
    // Fires a named event on `e`'s own graph through the installed sink, or does nothing (returns
    // false) if none is installed -- the built-in "FireEvent" action's whole implementation.
    bool fireNotify(scene::Entity e, const char* name) const;

    // Advances every CSynapseBehavior entity's tree by one tick. An entity with treeAssetId == 0,
    // or one whose id never resolved to a loaded tree, is silently skipped -- "no tree assigned
    // yet" is not an error, the same tolerance AgentSystem's own Requested-with-no-grid state has.
    void tick(scene::World& world, f32 dt);

    // The registry every Condition/Action leaf is resolved through. registerBuiltinBehaviors
    // (below) populates the seven built-ins into THIS; a project registers its own here too.
    BtRegistry& registry() { return registry_; }

    // How many entities currently hold live Running state. Exists so a test can assert the side
    // table does not leak once a behaviour resolves or its entity is destroyed, without reaching
    // into private state -- the same reason AgentSystem::activePathCount() exists.
    usize runningCount() const { return running_.size(); }

private:
    u32 type_ = 0;
    BtRegistry registry_;
    std::unordered_map<u64, fmt::OcBtData> trees_;
    // KEYED BY THE FULL ENTITY HANDLE, pruned every tick -- AgentSystem::paths_'s own fix, applied
    // here for the identical reason.
    std::unordered_map<scene::Entity, BtRunningState> running_;
    NotifyFn notify_ = nullptr;
    void* notifyUser_ = nullptr;

    void prune(scene::World& world);
};

// The process-global system, matching anim::animSystem() / synapse::agentSystem() /
// synapse::perceptionSystem().
BtSystem& btSystem();

// Registers the seven built-in Conditions/Actions into `system`'s own registry() --
//   Conditions: HasTarget, CanSeeTarget, DistanceToTargetLess (params[0] = threshold, cm)
//   Actions:    MoveTo (params[0..2] = world goal), Wait (params[0] = duration, sec), LookAt,
//               FireEvent (stringParam = the event name to raise)
// All five that read perception/pathing state do so through synapse::agentSystem() /
// synapse::perceptionSystem() directly -- the same singletons a graph node's own native relay
// reaches -- rather than through a `user` pointer, so there is nothing a composition root needs to
// pass in beyond calling this function once, after both of those systems' own registerComponents.
// A project's OWN named conditions/actions register into the SAME registry() afterward; nothing
// here assumes these seven are the only entries that will ever exist.
void registerBuiltinBehaviors(BtSystem& system);

} // namespace aver::synapse
