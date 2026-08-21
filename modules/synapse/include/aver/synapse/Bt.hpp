#pragma once
// The behaviour-tree evaluator: ticks a baked OcBtData against a registry of named
// Conditions/Actions, given an opaque subject and a place to keep Running state across ticks.
//
// PURE -- no scene::Entity, no World, for the identical reason Nav.hpp is: the tree format
// (Aver.Formats) and the tick algorithm here know nothing about what a "subject" is. Only the
// registered Condition/Action FUNCTIONS (installed by whoever owns a scene::World -- Aver.Synapse.
// Scene, not this module) interpret the subject as an entity and read or change real game state.
// That is what lets a hand-built tree and a pair of fake Condition/Action functions test the WHOLE
// algorithm -- composites, Cooldown, Running-state resume -- with no world at all.
#include "aver/core/Types.hpp"
#include "aver/formats/OcBt.hpp"

#include <string>
#include <unordered_map>

namespace aver::synapse {

enum class BtStatus : i32 { Running = 0, Success = 1, Failure = 2 };

// A Condition leaf: a pure query, Success or Failure only -- "am I currently true" has no Running
// state to report. `node` is the tree's own OcBtNode, so a condition reads its own params/name off
// it directly (e.g. the built-in "DistanceToTargetLess" reads node.params[0] as its threshold).
using BtConditionFn = bool (*)(i32 subject, const fmt::OcBtNode& node, void* user);

// An Action leaf: may run across multiple ticks (a Wait, a MoveTo). `elapsed` is a PERSISTENT
// float scratch slot -- one per action node per subject, owned and kept alive by the caller's own
// BtRunningState -- the only per-node memory an action gets for free. "Wait" accumulates dt into it
// directly; most others ignore it entirely.
using BtActionFn = BtStatus (*)(i32 subject, const fmt::OcBtNode& node, f32 dt, f32& elapsed, void* user);

// Named lookup for Condition/Action leaves. A project registers its OWN names here too -- nothing
// about this class assumes the seven built-ins (installed by Aver.Synapse.Scene, not here) are the
// only Conditions/Actions that will ever exist.
class BtRegistry {
public:
    struct ConditionEntry { BtConditionFn fn = nullptr; void* user = nullptr; };
    struct ActionEntry    { BtActionFn    fn = nullptr; void* user = nullptr; };

    void registerCondition(const std::string& name, BtConditionFn fn, void* user = nullptr) {
        conditions_[name] = ConditionEntry{fn, user};
    }
    void registerAction(const std::string& name, BtActionFn fn, void* user = nullptr) {
        actions_[name] = ActionEntry{fn, user};
    }

    // nullptr on a miss -- SILENTLY, matching scene::World::componentId's own "an unregistered name
    // is a valid, common query, not a crash" convention. tickBt is the one that logs on a miss,
    // matching aver::save's own "the lookup returns quietly; whoever asked for it warns" split.
    const ConditionEntry* findCondition(const std::string& name) const;
    const ActionEntry*    findAction(const std::string& name) const;

private:
    std::unordered_map<std::string, ConditionEntry> conditions_;
    std::unordered_map<std::string, ActionEntry>    actions_;
};

// Persistent state for ONE tree instance running against ONE subject. Own this yourself, keyed by
// whatever identifies "this subject, this tree" to YOU -- Aver.Synapse.Scene keys it by the full
// scene::Entity handle, pruned every tick it no longer applies. See AnimSystem::posed_'s own header
// comment (modules/anim.scene/include/aver/anim/AnimSystem.hpp) for why "the full handle, pruned"
// and not "the index, kept forever" is the only safe choice -- this struct has no opinion of its
// own and does no pruning; that is entirely the caller's job.
struct BtRunningState {
    // Per COMPOSITE node currently on the active running path: which CHILD node index to resume at
    // next tick, skipping already-resolved earlier siblings. A node absent here is not currently
    // running any part of its own subtree.
    std::unordered_map<i32, i32> runningChild;
    // Per Cooldown node: seconds remaining before it will run its child again. Independent of the
    // active-path concept above -- a Cooldown gates on wall-clock time, not on whether anything
    // else in the tree happens to be Running right now.
    std::unordered_map<i32, f32> cooldownRemaining;
    // Per Action node: the persistent scratch float BtActionFn's own `elapsed` parameter reads and
    // writes. Erased the moment a node resolves to Success or Failure, so a Wait run a second time
    // starts fresh rather than half-elapsed from its last run.
    std::unordered_map<i32, f32> actionElapsed;
};

// Ticks the tree rooted at tree.nodes[0] once, returning the ROOT's status and updating `state` in
// place. `dt` reaches every Action and every Cooldown timer; nothing else in the algorithm uses it.
// Failure (not a crash) for an empty tree -- OcBtData::valid() already refuses to let one exist on
// disk, but a caller building one by hand in a test is not obligated to have checked first.
BtStatus tickBt(const fmt::OcBtData& tree, const BtRegistry& registry, i32 subject, f32 dt,
                BtRunningState& state);

} // namespace aver::synapse
