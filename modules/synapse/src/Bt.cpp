#include "aver/synapse/Bt.hpp"

#include "aver/core/Log.hpp"

#include <algorithm>

namespace aver::synapse {
namespace {

using Kids = std::vector<std::vector<i32>>;

struct Ctx {
    const fmt::OcBtData&  tree;
    const BtRegistry&     registry;
    i32                   subject;
    f32                   dt;
    BtRunningState&       state;
    const BtDecoratorSet* decos;     // null when the tree has none
    Blackboard*           board;
    const Kids&           kids;
    bool                  track;     // maintain state.active
    std::unordered_set<i32> resuming; // nodes that were Running last tick (decorators are not re-checked)
};

BtStatus tickNode(Ctx& c, i32 idx);

// Selector and Sequence differ only in which child status short-circuits.
BtStatus tickComposite(Ctx& c, i32 idx, BtStatus shortCircuitOn, BtStatus allTookTheOtherStatus) {
    const std::vector<i32>& children = c.kids[static_cast<usize>(idx)];

    // Resume at the child that was Running rather than re-evaluating resolved siblings.
    usize startPos = 0;
    if (const auto it = c.state.runningChild.find(idx); it != c.state.runningChild.end()) {
        for (usize i = 0; i < children.size(); ++i)
            if (children[i] == it->second) { startPos = i; break; }
    }

    for (usize i = startPos; i < children.size(); ++i) {
        const BtStatus s = tickNode(c, children[i]);
        if (s == BtStatus::Running) {
            c.state.runningChild[idx] = children[i];
            return BtStatus::Running;
        }
        if (s == shortCircuitOn) {
            c.state.runningChild.erase(idx);
            return shortCircuitOn;
        }
    }
    c.state.runningChild.erase(idx);
    return allTookTheOtherStatus;
}

BtStatus tickNodeImpl(Ctx& c, i32 idx) {
    const fmt::OcBtNode& node = c.tree.nodes[static_cast<usize>(idx)];
    const std::vector<i32>& children = c.kids[static_cast<usize>(idx)];
    switch (node.kind) {
    case fmt::OcBtNodeKind::Selector:
        return tickComposite(c, idx, BtStatus::Success, BtStatus::Failure);

    case fmt::OcBtNodeKind::Sequence:
        return tickComposite(c, idx, BtStatus::Failure, BtStatus::Success);

    case fmt::OcBtNodeKind::Parallel: {
        // Every child ticks every tick, even once the answer is known, so a nested Sequence keeps advancing.
        bool anyRunning = false, anyFailure = false;
        for (const i32 ch : children) {
            const BtStatus s = tickNode(c, ch);
            if (s == BtStatus::Failure) anyFailure = true;
            else if (s == BtStatus::Running) anyRunning = true;
        }
        if (anyFailure) return BtStatus::Failure;
        if (anyRunning) return BtStatus::Running;
        return BtStatus::Success;
    }

    case fmt::OcBtNodeKind::Inverter: {
        if (children.empty()) return BtStatus::Failure;
        const BtStatus s = tickNode(c, children[0]);
        if (s == BtStatus::Success) return BtStatus::Failure;
        if (s == BtStatus::Failure) return BtStatus::Success;
        return BtStatus::Running;
    }

    case fmt::OcBtNodeKind::Succeeder: {
        if (children.empty()) return BtStatus::Success;
        const BtStatus s = tickNode(c, children[0]);
        return s == BtStatus::Running ? BtStatus::Running : BtStatus::Success;
    }

    case fmt::OcBtNodeKind::Cooldown: {
        if (children.empty()) return BtStatus::Failure;
        f32& remaining = c.state.cooldownRemaining[idx];   // a fresh Cooldown is immediately ready
        if (remaining > 0.0f) {
            remaining -= c.dt;
            return BtStatus::Failure;
        }
        const BtStatus s = tickNode(c, children[0]);
        if (s != BtStatus::Running) remaining = node.params[0];
        return s;
    }

    case fmt::OcBtNodeKind::Condition:
        if (const BtRegistry::ConditionEntry* e = c.registry.findCondition(node.name))
            return e->fn(c.subject, node, e->user) ? BtStatus::Success : BtStatus::Failure;
        AVER_WARN("[Synapse] Condition '{}' is not registered -- treated as Failure", node.name);
        return BtStatus::Failure;

    case fmt::OcBtNodeKind::Action:
        if (const BtRegistry::ActionEntry* e = c.registry.findAction(node.name)) {
            f32& elapsed = c.state.actionElapsed[idx];
            const BtStatus s = e->fn(c.subject, node, c.dt, elapsed, e->user);
            if (s != BtStatus::Running) c.state.actionElapsed.erase(idx);
            return s;
        }
        AVER_WARN("[Synapse] Action '{}' is not registered -- treated as Failure", node.name);
        return BtStatus::Failure;
    }
    return BtStatus::Failure;
}

BtStatus tickNode(Ctx& c, i32 idx) {
    bool gated = false;
    if (c.decos) {
        if (const std::vector<BtDecorator>* list = c.decos->find(idx)) {
            // Only on entry: a node that was already Running keeps going unless an abort cleared it.
            if (c.resuming.find(idx) == c.resuming.end()) {
                for (const BtDecorator& d : *list)
                    if (!evaluateDecorator(d, c.board)) { gated = true; break; }
            }
        }
    }

    const BtStatus s = gated ? BtStatus::Failure : tickNodeImpl(c, idx);

    if (c.track && s == BtStatus::Running) c.state.active.insert(idx);
    if (c.state.trace && static_cast<usize>(idx) < c.state.nodeTrace.size()) {
        BtNodeTrace& t = c.state.nodeTrace[static_cast<usize>(idx)];
        t.visited = true;
        t.status = s;
        t.tick = c.state.tickCount;
        t.gated = gated;
    }
    return s;
}

// ---- observer aborts -----------------------------------------------------------------------------

// Forgets running state in a subtree and tells any Running action it was aborted.
void clearSubtree(Ctx& c, i32 idx) {
    const fmt::OcBtNode& n = c.tree.nodes[static_cast<usize>(idx)];
    if (n.kind == fmt::OcBtNodeKind::Action && c.state.active.count(idx) != 0) {
        if (const BtRegistry::ActionEntry* e = c.registry.findAction(n.name))
            if (e->onAbort) e->onAbort(c.subject, n, e->user);
    }
    if (n.kind == fmt::OcBtNodeKind::Action) c.state.actionElapsed.erase(idx);
    c.state.runningChild.erase(idx);
    c.state.active.erase(idx);
    for (const i32 k : c.kids[static_cast<usize>(idx)]) clearSubtree(c, k);
}

void recordAbort(Ctx& c, i32 node) {
    ++c.state.abortCount;
    c.state.lastAbortedNode = node;
}

usize positionOf(const std::vector<i32>& list, i32 value) {
    for (usize i = 0; i < list.size(); ++i)
        if (list[i] == value) return i;
    return list.size();
}

// A decorator on `node` just became true while `node` is not running: if a lower-priority branch is
// running under a common Selector/Sequence, abort it so evaluation restarts at `node`.
void abortLowerPriority(Ctx& c, i32 node) {
    i32 child = node;
    for (i32 p = c.tree.nodes[static_cast<usize>(node)].parent; p != fmt::kOcBtNoParent;
         child = p, p = c.tree.nodes[static_cast<usize>(p)].parent) {
        const fmt::OcBtNodeKind k = c.tree.nodes[static_cast<usize>(p)].kind;
        if (k != fmt::OcBtNodeKind::Selector && k != fmt::OcBtNodeKind::Sequence) continue;
        if (c.state.active.count(p) == 0) continue;
        const auto it = c.state.runningChild.find(p);
        if (it == c.state.runningChild.end()) continue;

        const std::vector<i32>& siblings = c.kids[static_cast<usize>(p)];
        if (positionOf(siblings, child) < positionOf(siblings, it->second)) {
            const i32 victim = it->second;
            clearSubtree(c, victim);
            c.state.runningChild[p] = child;   // resume the composite at the branch holding `node`
            recordAbort(c, victim);
            return;
        }
    }
}

void applyObserverAborts(Ctx& c) {
    for (const auto& [node, list] : c.decos->byNode) {
        if (node < 0 || static_cast<usize>(node) >= c.tree.nodes.size()) continue;
        for (usize di = 0; di < list.size() && di < 16; ++di) {
            const BtDecorator& d = list[di];
            if (d.abort == BtAbortMode::None) continue;

            const u64 id = (static_cast<u64>(node) << 4) | static_cast<u64>(di);
            const u64 stampNow = c.board->stamp(d.key);
            const auto sIt = c.state.decoStamp.find(id);
            if (sIt != c.state.decoStamp.end() && sIt->second == stampNow) continue;   // key unchanged

            const bool cur = evaluateDecorator(d, c.board);
            const auto rIt = c.state.decoResult.find(id);
            const bool had = rIt != c.state.decoResult.end();
            const bool prev = had ? rIt->second : cur;
            c.state.decoStamp[id] = stampNow;
            c.state.decoResult[id] = cur;
            if (!had || prev == cur) continue;

            const bool selfAbort  = d.abort == BtAbortMode::Self || d.abort == BtAbortMode::Both;
            const bool lowerAbort = d.abort == BtAbortMode::LowerPriority || d.abort == BtAbortMode::Both;
            const bool running = c.state.active.count(node) != 0;
            if (running && !cur && selfAbort) {
                clearSubtree(c, node);
                recordAbort(c, node);
            } else if (!running && cur && lowerAbort) {
                abortLowerPriority(c, node);
            }
        }
    }
}

// ---- blackboard leaves ----------------------------------------------------------------------------

bool bbCompareCondition(i32 subject, const fmt::OcBtNode& node, void* user) {
    const auto* reg = static_cast<const BtRegistry*>(user);
    const Blackboard* b = reg ? reg->boardFor(subject) : nullptr;
    if (!b) return false;
    BbExpr e;
    if (!bbParseExprFor(node.stringParam, *b, e)) return false;
    const BbValue* actual = b->get(e.key);
    return actual && bbCompare(*actual, e.op, e.value);
}

BtStatus bbSetAction(i32 subject, const fmt::OcBtNode& node, f32, f32&, void* user) {
    const auto* reg = static_cast<const BtRegistry*>(user);
    Blackboard* b = reg ? reg->boardFor(subject) : nullptr;
    if (!b) return BtStatus::Failure;
    BbExpr e;
    if (!bbParseExprFor(node.stringParam, *b, e) || e.op != BbOp::Eq) return BtStatus::Failure;
    return b->set(e.key, e.value) ? BtStatus::Success : BtStatus::Failure;
}

BtStatus bbClearAction(i32 subject, const fmt::OcBtNode& node, f32, f32&, void* user) {
    const auto* reg = static_cast<const BtRegistry*>(user);
    Blackboard* b = reg ? reg->boardFor(subject) : nullptr;
    if (!b) return BtStatus::Failure;
    std::string key = node.stringParam;
    while (!key.empty() && key.back() == ' ') key.pop_back();
    while (!key.empty() && key.front() == ' ') key.erase(key.begin());
    return b->reset(key) ? BtStatus::Success : BtStatus::Failure;
}

} // namespace

// ---- public ---------------------------------------------------------------------------------------

const char* btAbortModeName(BtAbortMode m) {
    switch (m) {
        case BtAbortMode::None: return "None";
        case BtAbortMode::Self: return "Self";
        case BtAbortMode::LowerPriority: return "Lower Priority";
        case BtAbortMode::Both: return "Both";
    }
    return "?";
}

bool evaluateDecorator(const BtDecorator& d, const Blackboard* board) {
    if (!board) return false;
    const BbValue* v = board->get(d.key);
    return v && bbCompare(*v, d.op, d.value);
}

void BtRunningState::reset() {
    runningChild.clear();
    cooldownRemaining.clear();
    actionElapsed.clear();
    active.clear();
    decoStamp.clear();
    decoResult.clear();
    nodeTrace.clear();
    abortCount = 0;
    lastAbortedNode = -1;
}

const BtRegistry::ConditionEntry* BtRegistry::findCondition(const std::string& name) const {
    const auto it = conditions_.find(name);
    return it != conditions_.end() ? &it->second : nullptr;
}

const BtRegistry::ActionEntry* BtRegistry::findAction(const std::string& name) const {
    const auto it = actions_.find(name);
    return it != actions_.end() ? &it->second : nullptr;
}

void BtRegistry::registerBlackboardLeaves() {
    registerCondition("BbCompare", &bbCompareCondition, this);
    registerAction("BbSet", &bbSetAction, this);
    registerAction("BbClear", &bbClearAction, this);
}

std::vector<std::vector<i32>> btBuildChildren(const fmt::OcBtData& tree) {
    std::vector<std::vector<i32>> kids(tree.nodes.size());
    for (usize i = 0; i < tree.nodes.size(); ++i) {
        const i32 p = tree.nodes[i].parent;
        if (p >= 0 && static_cast<usize>(p) < kids.size()) kids[static_cast<usize>(p)].push_back(static_cast<i32>(i));
    }
    return kids;
}

BtStatus tickBt(const fmt::OcBtData& tree, const BtRegistry& registry, i32 subject, f32 dt,
                BtRunningState& state, const BtBlackboardCtx* bb) {
    if (tree.nodes.empty()) return BtStatus::Failure;

    Kids localKids;
    const Kids* kids = (bb && bb->children && bb->children->size() == tree.nodes.size()) ? bb->children : nullptr;
    if (!kids) { localKids = btBuildChildren(tree); kids = &localKids; }

    ++state.tickCount;
    if (state.trace && state.nodeTrace.size() != tree.nodes.size())
        state.nodeTrace.assign(tree.nodes.size(), BtNodeTrace{});

    const BtDecoratorSet* decos = (bb && bb->decorators && !bb->decorators->empty()) ? bb->decorators : nullptr;
    Ctx c{tree, registry, subject, dt, state, decos, bb ? bb->board : nullptr, *kids, decos != nullptr, {}};
    if (decos) {
        if (c.board) applyObserverAborts(c);
        c.resuming = state.active;   // what was Running, after aborts removed their subtrees
        state.active.clear();
    }
    return tickNode(c, 0);
}

} // namespace aver::synapse
