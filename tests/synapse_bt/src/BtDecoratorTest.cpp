// Blackboard decorators on the behaviour-tree evaluator: entry gating, observer aborts (Self,
// LowerPriority, Both), action abort callbacks, shared-key observation across agents, the BbCompare /
// BbSet / BbClear leaves and the per-node trace. Pure: hand-built trees and fake leaves, no world.
#include "aver/synapse/Bt.hpp"

#include "aver/core/Log.hpp"

#include <string>

using namespace aver;
using namespace aver::synapse;

static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

static int g_work = 0, g_idle = 0, g_abortedWork = 0, g_abortedIdle = 0;

static BtStatus workAction(i32, const fmt::OcBtNode&, f32, f32&, void*) { ++g_work; return BtStatus::Running; }
static BtStatus idleAction(i32, const fmt::OcBtNode&, f32, f32&, void*) { ++g_idle; return BtStatus::Running; }
static void workAbort(i32, const fmt::OcBtNode&, void*) { ++g_abortedWork; }
static void idleAbort(i32, const fmt::OcBtNode&, void*) { ++g_abortedIdle; }

static void resetCounters() { g_work = g_idle = g_abortedWork = g_abortedIdle = 0; }

static BtRegistry makeRegistry() {
    BtRegistry r;
    r.registerAction("Work", &workAction, nullptr, &workAbort);
    r.registerAction("Idle", &idleAction, nullptr, &idleAbort);
    return r;
}

static fmt::OcBtNode mk(fmt::OcBtNodeKind kind, i32 parent, const char* name = "") {
    fmt::OcBtNode n;
    n.kind = kind;
    n.parent = parent;
    n.name = name;
    return n;
}

// 0 Selector -> [1 Sequence -> [2 Work], 3 Idle]. Node 1 carries the decorator.
static fmt::OcBtData makeTree() {
    fmt::OcBtData t;
    t.nodes.push_back(mk(fmt::OcBtNodeKind::Selector, fmt::kOcBtNoParent));
    t.nodes.push_back(mk(fmt::OcBtNodeKind::Sequence, 0));
    t.nodes.push_back(mk(fmt::OcBtNodeKind::Action, 1, "Work"));
    t.nodes.push_back(mk(fmt::OcBtNodeKind::Action, 0, "Idle"));
    return t;
}

static BtDecoratorSet alertDecorator(BtAbortMode mode) {
    BtDecorator d;
    d.key = "Alert";
    d.op = BbOp::IsSet;
    d.abort = mode;
    BtDecoratorSet s;
    s.byNode[1].push_back(d);
    return s;
}

static void defineAlert(Blackboard& b, BbScope scope = BbScope::Agent) {
    BbKeyDef k;
    k.name = "Alert";
    k.type = BbType::Bool;
    k.scope = scope;
    b.defineKey(k);
}

static BtStatus tick(const fmt::OcBtData& t, const BtRegistry& r, BtRunningState& s, const BtDecoratorSet& d,
                     Blackboard& b, i32 subject = 1) {
    const BtBlackboardCtx ctx{&d, &b, nullptr};
    return tickBt(t, r, subject, 0.016f, s, &ctx);
}

// The resolver used by the blackboard leaves.
static Blackboard* g_leafBoard = nullptr;
static Blackboard* resolveLeafBoard(i32, void*) { return g_leafBoard; }

int main() {
    AVER_INFO("BtDecoratorTest");
    const fmt::OcBtData tree = makeTree();
    const BtRegistry reg = makeRegistry();

    {
        AVER_INFO("entry gating");
        resetCounters();
        Blackboard b; defineAlert(b);
        BtRunningState st;
        const BtDecoratorSet d = alertDecorator(BtAbortMode::None);
        tick(tree, reg, st, d, b);
        check(g_work == 0 && g_idle == 1, "with Alert false the decorated branch is refused and the next one runs");
        b.setBool("Alert", true);
        tick(tree, reg, st, d, b);
        check(g_work == 0 && g_idle == 2, "a running lower branch is NOT interrupted when the decorator has no abort mode");

        resetCounters();
        Blackboard b2; defineAlert(b2); b2.setBool("Alert", true);
        BtRunningState st2;
        tick(tree, reg, st2, d, b2);
        check(g_work == 1 && g_idle == 0, "with Alert true the branch is entered");
        b2.setBool("Alert", false);
        tick(tree, reg, st2, d, b2);
        check(g_work == 2 && g_abortedWork == 0, "abort None: a running branch keeps going after its condition turns false");

        Blackboard none;
        BtRunningState st3;
        resetCounters();
        const BtStatus s = tick(tree, reg, st3, d, none);
        check(s == BtStatus::Running && g_work == 0 && g_idle == 1, "an undefined key counts as false");
    }

    {
        AVER_INFO("Self abort");
        resetCounters();
        Blackboard b; defineAlert(b); b.setBool("Alert", true);
        BtRunningState st;
        const BtDecoratorSet d = alertDecorator(BtAbortMode::Self);
        tick(tree, reg, st, d, b);
        check(g_work == 1, "the branch is entered while Alert holds");
        b.setBool("Alert", false);
        tick(tree, reg, st, d, b);
        check(g_abortedWork == 1, "turning the key false aborts the running action (its abort callback ran)");
        check(g_work == 1 && g_idle == 1, "and the same tick falls through to the next branch");
        check(st.abortCount == 1 && st.lastAbortedNode == 1, "the state records the abort");
        tick(tree, reg, st, d, b);
        check(st.abortCount == 1 && g_idle == 2, "a key that stays false does not abort again");
        b.setBool("Alert", true);
        tick(tree, reg, st, d, b);
        check(g_work == 1, "Self alone does not pull a running lower branch back");
    }

    {
        AVER_INFO("Lower priority abort");
        resetCounters();
        Blackboard b; defineAlert(b);
        BtRunningState st;
        const BtDecoratorSet d = alertDecorator(BtAbortMode::LowerPriority);
        tick(tree, reg, st, d, b);
        check(g_idle == 1 && g_work == 0, "the low-priority branch runs while Alert is false");
        b.setBool("Alert", true);
        tick(tree, reg, st, d, b);
        check(g_abortedIdle == 1, "Alert turning true aborts the running lower-priority action");
        check(g_work == 1, "and the higher-priority branch starts in the same tick");
        check(st.lastAbortedNode == 3, "the aborted node is the lower-priority one");
        b.setBool("Alert", false);
        tick(tree, reg, st, d, b);
        check(g_abortedWork == 0 && g_work == 2, "LowerPriority alone does not abort its own branch");
    }

    {
        AVER_INFO("Both");
        resetCounters();
        Blackboard b; defineAlert(b);
        BtRunningState st;
        const BtDecoratorSet d = alertDecorator(BtAbortMode::Both);
        tick(tree, reg, st, d, b);
        b.setBool("Alert", true);
        tick(tree, reg, st, d, b);
        check(g_work == 1 && g_abortedIdle == 1, "true: the branch pre-empts the idle one");
        b.setBool("Alert", false);
        tick(tree, reg, st, d, b);
        check(g_abortedWork == 1 && g_idle == 2, "false: the branch is dropped for idle");
        b.setBool("Alert", true);
        tick(tree, reg, st, d, b);
        check(g_work == 2 && g_abortedIdle == 2, "true again: it pre-empts again");
        check(st.abortCount == 3, "three aborts in total");
    }

    {
        AVER_INFO("a shared key aborts every agent on the team and nobody else");
        resetCounters();
        SharedBlackboards teams;
        Blackboard a, b, other;
        a.setShared(&teams.get(1));
        b.setShared(&teams.get(1));
        other.setShared(&teams.get(2));
        defineAlert(a, BbScope::Shared);
        defineAlert(b, BbScope::Shared);
        defineAlert(other, BbScope::Shared);
        BtRunningState sa, sb, so;
        const BtDecoratorSet d = alertDecorator(BtAbortMode::LowerPriority);
        tick(tree, reg, sa, d, a, 1);
        tick(tree, reg, sb, d, b, 2);
        tick(tree, reg, so, d, other, 3);
        check(g_idle == 3, "all three start idle");
        resetCounters();
        a.setBool("Alert", true);
        tick(tree, reg, sa, d, a, 1);
        tick(tree, reg, sb, d, b, 2);
        tick(tree, reg, so, d, other, 3);
        check(g_work == 2 && g_abortedIdle == 2, "both teammates switch to the alert branch");
        check(g_idle == 1, "the other team keeps idling");
    }

    {
        AVER_INFO("the blackboard leaves");
        Blackboard b;
        BbKeyDef k;
        k.name = "Count"; k.type = BbType::Int;
        b.defineKey(k);
        g_leafBoard = &b;
        BtRegistry r;
        r.setBoardResolver(&resolveLeafBoard, nullptr);
        r.registerBlackboardLeaves();

        fmt::OcBtData t;
        t.nodes.push_back(mk(fmt::OcBtNodeKind::Sequence, fmt::kOcBtNoParent));
        t.nodes.push_back(mk(fmt::OcBtNodeKind::Condition, 0, "BbCompare"));
        t.nodes.back().stringParam = "Count < 3";
        t.nodes.push_back(mk(fmt::OcBtNodeKind::Action, 0, "BbSet"));
        t.nodes.back().stringParam = "Count = 5";
        BtRunningState st;
        check(tickBt(t, r, 1, 0.0f, st) == BtStatus::Success, "BbCompare passes, then BbSet runs");
        check(b.getInt("Count") == 5, "BbSet wrote the key");
        check(tickBt(t, r, 1, 0.0f, st) == BtStatus::Failure, "the comparison now fails");

        fmt::OcBtData c;
        c.nodes.push_back(mk(fmt::OcBtNodeKind::Action, fmt::kOcBtNoParent, "BbClear"));
        c.nodes[0].stringParam = "Count";
        check(tickBt(c, r, 1, 0.0f, st) == BtStatus::Success && b.getInt("Count") == 0, "BbClear resets to the default");
        c.nodes[0].stringParam = "Nope";
        check(tickBt(c, r, 1, 0.0f, st) == BtStatus::Failure, "BbClear on an unknown key fails");

        fmt::OcBtData bad;
        bad.nodes.push_back(mk(fmt::OcBtNodeKind::Action, fmt::kOcBtNoParent, "BbSet"));
        bad.nodes[0].stringParam = "Count = banana";
        check(tickBt(bad, r, 1, 0.0f, st) == BtStatus::Failure, "BbSet with a value of the wrong type fails");
        g_leafBoard = nullptr;
        check(tickBt(bad, r, 1, 0.0f, st) == BtStatus::Failure, "and with no board at all");
    }

    {
        AVER_INFO("per-node trace");
        resetCounters();
        Blackboard b; defineAlert(b);
        BtRunningState st;
        st.trace = true;
        const BtDecoratorSet d = alertDecorator(BtAbortMode::None);
        tick(tree, reg, st, d, b);
        check(st.nodeTrace.size() == 4, "one trace slot per node");
        check(st.nodeTrace[1].visited && st.nodeTrace[1].gated && st.nodeTrace[1].status == BtStatus::Failure,
              "the gated branch is recorded as refused");
        check(!st.nodeTrace[2].visited, "its child was never reached");
        check(st.nodeTrace[3].status == BtStatus::Running && st.nodeTrace[3].tick == st.tickCount,
              "the running leaf is stamped with this tick");
        check(st.nodeTrace[0].status == BtStatus::Running, "the root reports Running");
    }

    {
        AVER_INFO("no context means no decorators");
        resetCounters();
        BtRunningState st;
        check(tickBt(tree, reg, 1, 0.0f, st) == BtStatus::Running && g_work == 1,
              "without a blackboard context the tree ticks as before");
        st.reset();
        check(st.runningChild.empty() && st.active.empty(), "reset forgets running state");
    }

    AVER_INFO(g_failures ? "BtDecoratorTest: {} FAILURES" : "BtDecoratorTest: all checks passed ({})", g_failures);
    return g_failures ? 1 : 0;
}
