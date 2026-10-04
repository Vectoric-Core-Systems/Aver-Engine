// The behaviour-tree evaluator: composites, Cooldown, and Running state that resumes at the same
// child instead of restarting the whole subtree -- against hand-built trees and fake
// Condition/Action functions, so the algorithm is checked with no scene and no world at all. See
// Bt.hpp's own header for why purity here is the whole point, the same reason NavTest.cpp gives.
#include "aver/synapse/Bt.hpp"

#include "aver/core/Log.hpp"
#include "aver/formats/OcBt.hpp"

#include <string>
#include <vector>

using namespace aver;
using aver::synapse::BtStatus;

static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

// ---- fake Condition/Action functions, controllable and counting -------------------------------

static int g_alwaysSucceedCalls = 0;
static synapse::BtStatus alwaysSucceedAction(i32, const fmt::OcBtNode&, f32, f32&, void*) {
    ++g_alwaysSucceedCalls;
    return BtStatus::Success;
}

static synapse::BtStatus alwaysFailAction(i32, const fmt::OcBtNode&, f32, f32&, void*) {
    return BtStatus::Failure;
}

// Mirrors what the real built-in "Wait" (Aver.Synapse.Scene) will do: accumulate dt into the
// per-node scratch slot, Success once node.params[0] seconds have elapsed.
static synapse::BtStatus testWaitAction(i32, const fmt::OcBtNode& node, f32 dt, f32& elapsed, void*) {
    elapsed += dt;
    if (elapsed >= node.params[0]) { elapsed = 0.0f; return BtStatus::Success; }
    return BtStatus::Running;
}

static int g_cooldownActionCalls = 0;
static synapse::BtStatus countingAction(i32, const fmt::OcBtNode&, f32, f32&, void*) {
    ++g_cooldownActionCalls;
    return BtStatus::Success;
}

int main() {
    AVER_INFO("BtTest");

    AVER_INFO("a hand-built tree round-trips through write and parse byte-for-byte in meaning");
    {
        fmt::OcBtData tree;
        tree.nodes.push_back({fmt::OcBtNodeKind::Sequence, fmt::kOcBtNoParent, "", {0, 0, 0, 0}});
        fmt::OcBtNode cond;
        cond.kind = fmt::OcBtNodeKind::Condition;
        cond.parent = 0;
        cond.name = "HasTarget";
        tree.nodes.push_back(cond);
        fmt::OcBtNode act;
        act.kind = fmt::OcBtNodeKind::Action;
        act.parent = 0;
        act.name = "Wait";
        act.params[0] = 1.5f;
        tree.nodes.push_back(act);

        check(tree.valid(), "the hand-built tree is valid before it ever touches a file");

        std::vector<u8> bytes;
        std::string why;
        check(fmt::writeOcBt(tree, bytes, &why), "it writes: " + why);

        fmt::OcBtData back;
        check(fmt::parseOcBt(bytes.data(), bytes.size(), back, &why), "and parses back: " + why);
        check(back.nodes.size() == 3, "the same node count came back");
        if (back.nodes.size() == 3) {
            check(back.nodes[0].kind == fmt::OcBtNodeKind::Sequence, "node 0 kind survived");
            check(back.nodes[1].name == "HasTarget", "node 1's name survived through the string table");
            check(back.nodes[2].name == "Wait" && back.nodes[2].params[0] == 1.5f,
                  "node 2's name AND its float param both survived");
        }
    }

    AVER_INFO("a Sequence containing a Wait returns Running across ticks and resumes at the SAME child");
    {
        g_alwaysSucceedCalls = 0;
        fmt::OcBtData tree;
        tree.nodes.push_back({fmt::OcBtNodeKind::Sequence, fmt::kOcBtNoParent, "", {}});
        fmt::OcBtNode first; first.kind = fmt::OcBtNodeKind::Action; first.parent = 0; first.name = "AlwaysSucceed";
        tree.nodes.push_back(first);
        fmt::OcBtNode wait; wait.kind = fmt::OcBtNodeKind::Action; wait.parent = 0; wait.name = "TestWait";
        wait.params[0] = 0.5f;
        tree.nodes.push_back(wait);
        fmt::OcBtNode last; last.kind = fmt::OcBtNodeKind::Action; last.parent = 0; last.name = "AlwaysSucceed";
        tree.nodes.push_back(last);
        check(tree.valid(), "this tree is valid");

        synapse::BtRegistry reg;
        reg.registerAction("AlwaysSucceed", &alwaysSucceedAction);
        reg.registerAction("TestWait", &testWaitAction);

        synapse::BtRunningState state;
        const f32 dt = 0.2f;

        const BtStatus t1 = synapse::tickBt(tree, reg, /*subject*/ 1, dt, state);
        check(t1 == BtStatus::Running, "tick 1: still Running (Wait needs 0.5s, only 0.2s elapsed)");
        check(g_alwaysSucceedCalls == 1,
              "AND THE FIRST 'AlwaysSucceed' RAN EXACTLY ONCE -- proves the Sequence started at the beginning");

        const BtStatus t2 = synapse::tickBt(tree, reg, 1, dt, state);
        check(t2 == BtStatus::Running, "tick 2: still Running (0.4s elapsed)");
        check(g_alwaysSucceedCalls == 1,
              "AND 'AlwaysSucceed' WAS NOT CALLED AGAIN -- the Sequence RESUMED at Wait, it did not restart");

        const BtStatus t3 = synapse::tickBt(tree, reg, 1, dt, state);
        check(t3 == BtStatus::Success,
              "tick 3: the whole Sequence succeeds (0.6s elapsed, Wait resolves, the last child runs and succeeds)");
        check(g_alwaysSucceedCalls == 2, "the LAST 'AlwaysSucceed' ran once the Wait resolved");
    }

    AVER_INFO("a Selector tries the next child on Failure, and stops at the first Success");
    {
        fmt::OcBtData tree;
        tree.nodes.push_back({fmt::OcBtNodeKind::Selector, fmt::kOcBtNoParent, "", {}});
        fmt::OcBtNode a; a.kind = fmt::OcBtNodeKind::Action; a.parent = 0; a.name = "AlwaysFail";
        tree.nodes.push_back(a);
        fmt::OcBtNode b; b.kind = fmt::OcBtNodeKind::Action; b.parent = 0; b.name = "AlwaysSucceed";
        tree.nodes.push_back(b);

        synapse::BtRegistry reg;
        reg.registerAction("AlwaysFail", &alwaysFailAction);
        reg.registerAction("AlwaysSucceed", &alwaysSucceedAction);

        synapse::BtRunningState state;
        const BtStatus s = synapse::tickBt(tree, reg, 2, 0.1f, state);
        check(s == BtStatus::Success, "the Selector reports Success from its second child");
    }

    AVER_INFO("Cooldown refuses to re-run its child until its timer elapses");
    {
        g_cooldownActionCalls = 0;
        fmt::OcBtData tree;
        fmt::OcBtNode root; root.kind = fmt::OcBtNodeKind::Cooldown; root.parent = fmt::kOcBtNoParent;
        root.params[0] = 1.0f;
        tree.nodes.push_back(root);
        fmt::OcBtNode child; child.kind = fmt::OcBtNodeKind::Action; child.parent = 0; child.name = "Counting";
        tree.nodes.push_back(child);

        synapse::BtRegistry reg;
        reg.registerAction("Counting", &countingAction);
        synapse::BtRunningState state;

        synapse::tickBt(tree, reg, 3, 0.3f, state);   // first tick: cooldown starts at 0, runs immediately
        check(g_cooldownActionCalls == 1, "the action ran on the very first tick");

        // 4 more ticks (1.2s of dt): remaining goes 1.0 -> 0.7 -> 0.4 -> 0.1 -> -0.2, but the CHECK
        // happens before the decrement each tick, so a still-positive 0.1 on tick 5 still refuses --
        // the cooldown reads as elapsed only on the NEXT tick after it first goes non-positive.
        for (int i = 0; i < 4; ++i) synapse::tickBt(tree, reg, 3, 0.3f, state);
        check(g_cooldownActionCalls == 1,
              "still only once after 1.2s of dt -- the timer went negative mid-tick, not before it");

        synapse::tickBt(tree, reg, 3, 0.3f, state);   // one more tick: remaining was -0.2, now runs again
        check(g_cooldownActionCalls == 2,
              "AND RAN AGAIN ONLY ONCE THE 1-SECOND COOLDOWN HAD FULLY ELAPSED, not once per tick");
    }

    AVER_INFO(g_failures ? "BtTest: {} FAILURES" : "BtTest: all checks passed ({})", g_failures);
    return g_failures ? 1 : 0;
}
