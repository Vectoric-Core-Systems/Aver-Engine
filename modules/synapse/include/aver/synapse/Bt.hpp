#pragma once
// The behaviour-tree evaluator: ticks a baked OcBtData against a registry of named
// Conditions/Actions, given an opaque subject and a place to keep Running state across ticks.
//
// PURE -- no scene::Entity, no World. Only the registered Condition/Action functions (installed by
// whoever owns a scene::World) interpret the subject. A hand-built tree and fake leaf functions test
// the whole algorithm with no world at all.
//
// Blackboard decorators and observer aborts (docs/BLACKBOARD_BT.md) are an optional layer: pass a
// BtBlackboardCtx to tickBt. Without one, ticking behaves exactly as before.
#include "aver/core/Types.hpp"
#include "aver/formats/OcBt.hpp"
#include "aver/synapse/Blackboard.hpp"

#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace aver::synapse {

enum class BtStatus : i32 { Running = 0, Success = 1, Failure = 2 };

// A Condition leaf: a pure query, Success or Failure only. `node` is the tree's own OcBtNode.
using BtConditionFn = bool (*)(i32 subject, const fmt::OcBtNode& node, void* user);

// An Action leaf: may run across multiple ticks. `elapsed` is a persistent float scratch slot, one per
// action node per subject, owned by the caller's BtRunningState.
using BtActionFn = BtStatus (*)(i32 subject, const fmt::OcBtNode& node, f32 dt, f32& elapsed, void* user);

// Called when a Running action is aborted by an observer abort, so it can stop what it started.
using BtActionAbortFn = void (*)(i32 subject, const fmt::OcBtNode& node, void* user);

// Resolves a subject's blackboard, for the BbCompare / BbSet / BbClear leaves.
using BtBoardResolverFn = Blackboard* (*)(i32 subject, void* user);

class BtRegistry {
public:
    struct ConditionEntry { BtConditionFn fn = nullptr; void* user = nullptr; };
    struct ActionEntry    { BtActionFn fn = nullptr; void* user = nullptr; BtActionAbortFn onAbort = nullptr; };

    void registerCondition(const std::string& name, BtConditionFn fn, void* user = nullptr) {
        conditions_[name] = ConditionEntry{fn, user};
    }
    void registerAction(const std::string& name, BtActionFn fn, void* user = nullptr,
                        BtActionAbortFn onAbort = nullptr) {
        actions_[name] = ActionEntry{fn, user, onAbort};
    }

    // nullptr on a miss, silently; tickBt logs.
    const ConditionEntry* findCondition(const std::string& name) const;
    const ActionEntry*    findAction(const std::string& name) const;

    void setBoardResolver(BtBoardResolverFn fn, void* user) { resolver_ = fn; resolverUser_ = user; }
    Blackboard* boardFor(i32 subject) const { return resolver_ ? resolver_(subject, resolverUser_) : nullptr; }

    // Registers the blackboard leaves (names below). Needs setBoardResolver first. The registry must
    // not be moved or copied afterwards: the leaves hold its address.
    //   Condition "BbCompare": stringParam is an expression: "key", "!key" or "key <op> value"
    //   Action    "BbSet":     stringParam "key = value" (Success; Failure on an unknown key/bad value)
    //   Action    "BbClear":   stringParam "key" (resets the key to its default)
    void registerBlackboardLeaves();

private:
    std::unordered_map<std::string, ConditionEntry> conditions_;
    std::unordered_map<std::string, ActionEntry>    actions_;
    BtBoardResolverFn resolver_ = nullptr;
    void*             resolverUser_ = nullptr;
};

// ---- blackboard decorators ------------------------------------------------------------------------

// What a decorator does when its condition changes while the tree is running:
//   Self           aborts the decorated subtree if it is running and the condition turns false
//   LowerPriority  when the condition turns true, aborts a running branch that sits LATER (lower
//                  priority) under a common Selector/Sequence and restarts at this node
//   Both           both of the above
enum class BtAbortMode : u8 { None = 0, Self = 1, LowerPriority = 2, Both = 3 };

const char* btAbortModeName(BtAbortMode m);

// A condition on a blackboard key, attached to any node. All of a node's decorators must pass for the
// node to be entered; with an abort mode they are also watched while it runs.
struct BtDecorator {
    std::string key;
    BbOp        op = BbOp::IsSet;
    BbValue     value;
    BtAbortMode abort = BtAbortMode::None;
};

// False when the board is null or the key is undefined.
bool evaluateDecorator(const BtDecorator& d, const Blackboard* board);

// Decorators by node index; a std::map so iteration order (and therefore abort order) is deterministic.
struct BtDecoratorSet {
    std::map<i32, std::vector<BtDecorator>> byNode;
    bool empty() const { return byNode.empty(); }
    const std::vector<BtDecorator>* find(i32 node) const {
        const auto it = byNode.find(node);
        return it == byNode.end() ? nullptr : &it->second;
    }
};

// Optional inputs to tickBt.
struct BtBlackboardCtx {
    const BtDecoratorSet* decorators = nullptr;
    Blackboard*           board = nullptr;
    // Precomputed child lists (index -> children in order); built per tick when null.
    const std::vector<std::vector<i32>>* children = nullptr;
};

// ---- running state --------------------------------------------------------------------------------

struct BtNodeTrace {
    bool     visited = false;
    BtStatus status = BtStatus::Failure;   // last result
    u64      tick = 0;                      // BtRunningState::tickCount at the last visit
    bool     gated = false;                 // last visit was refused by a decorator
};

// Persistent state for ONE tree instance running against ONE subject. The caller owns it and decides
// its key; this struct does no pruning.
struct BtRunningState {
    // Per composite node on the active path: which child to resume at.
    std::unordered_map<i32, i32> runningChild;
    // Per Cooldown node: seconds remaining before it will run its child again.
    std::unordered_map<i32, f32> cooldownRemaining;
    // Per Action node: the scratch float; erased when the node resolves.
    std::unordered_map<i32, f32> actionElapsed;

    // Nodes that returned Running last tick. Maintained only while decorators are in use.
    std::unordered_set<i32> active;
    // Last observed stamp / result per decorator (node * 16 + index), for flip detection.
    std::unordered_map<u64, u64>  decoStamp;
    std::unordered_map<u64, bool> decoResult;

    u64 tickCount = 0;
    u32 abortCount = 0;
    i32 lastAbortedNode = -1;

    // Per-node last results for the editor's live view; filled only while `trace` is set.
    bool trace = false;
    std::vector<BtNodeTrace> nodeTrace;

    // Forgets every running piece (after the tree changed shape); keeps the trace flag.
    void reset();
};

// Ticks the tree rooted at tree.nodes[0] once, returning the ROOT's status and updating `state`.
// Failure for an empty tree.
BtStatus tickBt(const fmt::OcBtData& tree, const BtRegistry& registry, i32 subject, f32 dt,
                BtRunningState& state, const BtBlackboardCtx* bb = nullptr);

// children[i] = indexes of node i's children, in execution order.
std::vector<std::vector<i32>> btBuildChildren(const fmt::OcBtData& tree);

} // namespace aver::synapse
