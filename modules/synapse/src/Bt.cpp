#include "aver/synapse/Bt.hpp"

#include "aver/core/Log.hpp"

namespace aver::synapse {
namespace {

std::vector<i32> childrenOf(const fmt::OcBtData& tree, i32 parentIndex) {
    std::vector<i32> out;
    for (i32 i = 0; i < static_cast<i32>(tree.nodes.size()); ++i)
        if (tree.nodes[static_cast<usize>(i)].parent == parentIndex) out.push_back(i);
    return out;
}

BtStatus tickNode(const fmt::OcBtData& tree, const BtRegistry& registry, i32 subject, f32 dt,
                  BtRunningState& state, i32 nodeIndex);

// Shared by Selector and Sequence -- they differ only in which child status short-circuits the
// whole composite, and what "every child took the other status" resolves to.
BtStatus tickComposite(const fmt::OcBtData& tree, const BtRegistry& registry, i32 subject, f32 dt,
                       BtRunningState& state, i32 nodeIndex, BtStatus shortCircuitOn,
                       BtStatus allTookTheOtherStatus) {
    const std::vector<i32> children = childrenOf(tree, nodeIndex);

    // RESUME, not restart: if this composite was Running last tick, pick up at the SAME child --
    // the whole point of the running-state table -- rather than re-evaluating already-resolved
    // earlier siblings.
    usize startPos = 0;
    if (const auto it = state.runningChild.find(nodeIndex); it != state.runningChild.end()) {
        for (usize i = 0; i < children.size(); ++i)
            if (children[i] == it->second) { startPos = i; break; }
    }

    for (usize i = startPos; i < children.size(); ++i) {
        const BtStatus s = tickNode(tree, registry, subject, dt, state, children[i]);
        if (s == BtStatus::Running) {
            state.runningChild[nodeIndex] = children[i];
            return BtStatus::Running;
        }
        if (s == shortCircuitOn) {
            state.runningChild.erase(nodeIndex);
            return shortCircuitOn;
        }
        // The child took the "keep going" status -- move to the next sibling.
    }
    state.runningChild.erase(nodeIndex);
    return allTookTheOtherStatus;
}

BtStatus tickNode(const fmt::OcBtData& tree, const BtRegistry& registry, i32 subject, f32 dt,
                  BtRunningState& state, i32 nodeIndex) {
    const fmt::OcBtNode& node = tree.nodes[static_cast<usize>(nodeIndex)];
    switch (node.kind) {
    case fmt::OcBtNodeKind::Selector:
        // First child to Succeed wins; Failure only once every child has failed.
        return tickComposite(tree, registry, subject, dt, state, nodeIndex,
                             BtStatus::Success, BtStatus::Failure);

    case fmt::OcBtNodeKind::Sequence:
        // First child to Fail wins; Success only once every child has succeeded.
        return tickComposite(tree, registry, subject, dt, state, nodeIndex,
                             BtStatus::Failure, BtStatus::Success);

    case fmt::OcBtNodeKind::Parallel: {
        // EVERY child ticked, EVERY tick, even after this node's own answer is already known -- a
        // sibling's own Running state (a nested Sequence mid-Wait, say) must keep advancing rather
        // than freeze because this Parallel already knows it will report Failure or Success.
        bool anyRunning = false, anyFailure = false;
        for (const i32 c : childrenOf(tree, nodeIndex)) {
            const BtStatus s = tickNode(tree, registry, subject, dt, state, c);
            if (s == BtStatus::Failure) anyFailure = true;
            else if (s == BtStatus::Running) anyRunning = true;
        }
        if (anyFailure) return BtStatus::Failure;
        if (anyRunning) return BtStatus::Running;
        return BtStatus::Success;
    }

    case fmt::OcBtNodeKind::Inverter: {
        const std::vector<i32> children = childrenOf(tree, nodeIndex);
        if (children.empty()) return BtStatus::Failure;   // malformed, but must not crash
        const BtStatus s = tickNode(tree, registry, subject, dt, state, children[0]);
        if (s == BtStatus::Success) return BtStatus::Failure;
        if (s == BtStatus::Failure) return BtStatus::Success;
        return BtStatus::Running;
    }

    case fmt::OcBtNodeKind::Succeeder: {
        const std::vector<i32> children = childrenOf(tree, nodeIndex);
        if (children.empty()) return BtStatus::Success;
        const BtStatus s = tickNode(tree, registry, subject, dt, state, children[0]);
        return s == BtStatus::Running ? BtStatus::Running : BtStatus::Success;
    }

    case fmt::OcBtNodeKind::Cooldown: {
        const std::vector<i32> children = childrenOf(tree, nodeIndex);
        if (children.empty()) return BtStatus::Failure;
        // Default-constructs to 0.0f on first use, so a fresh Cooldown is immediately ready.
        f32& remaining = state.cooldownRemaining[nodeIndex];
        if (remaining > 0.0f) {
            remaining -= dt;
            return BtStatus::Failure;   // still cooling down -- the child is not ticked at all
        }
        const BtStatus s = tickNode(tree, registry, subject, dt, state, children[0]);
        if (s != BtStatus::Running) remaining = node.params[0];   // reset the timer once resolved
        return s;
    }

    case fmt::OcBtNodeKind::Condition:
        if (const BtRegistry::ConditionEntry* e = registry.findCondition(node.name))
            return e->fn(subject, node, e->user) ? BtStatus::Success : BtStatus::Failure;
        AVER_WARN("[Synapse] Condition '{}' is not registered -- treated as Failure", node.name);
        return BtStatus::Failure;

    case fmt::OcBtNodeKind::Action:
        if (const BtRegistry::ActionEntry* e = registry.findAction(node.name)) {
            f32& elapsed = state.actionElapsed[nodeIndex];   // 0.0f on first use
            const BtStatus s = e->fn(subject, node, dt, elapsed, e->user);
            if (s != BtStatus::Running) state.actionElapsed.erase(nodeIndex);   // clean slate next run
            return s;
        }
        AVER_WARN("[Synapse] Action '{}' is not registered -- treated as Failure", node.name);
        return BtStatus::Failure;
    }
    return BtStatus::Failure;   // unreachable -- every OcBtNodeKind value is handled above
}

} // namespace

const BtRegistry::ConditionEntry* BtRegistry::findCondition(const std::string& name) const {
    const auto it = conditions_.find(name);
    return it != conditions_.end() ? &it->second : nullptr;
}

const BtRegistry::ActionEntry* BtRegistry::findAction(const std::string& name) const {
    const auto it = actions_.find(name);
    return it != actions_.end() ? &it->second : nullptr;
}

BtStatus tickBt(const fmt::OcBtData& tree, const BtRegistry& registry, i32 subject, f32 dt,
                BtRunningState& state) {
    if (tree.nodes.empty()) return BtStatus::Failure;
    return tickNode(tree, registry, subject, dt, state, 0);
}

} // namespace aver::synapse
