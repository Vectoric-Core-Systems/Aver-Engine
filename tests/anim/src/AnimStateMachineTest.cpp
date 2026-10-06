// Animation state machines: transition timing, blend weights, events, sub-machines and parameters,
// with no scene and no GPU.
//
// Everything here is a property of a clock and a list of conditions. A transition that fires one
// tick late, a blend whose weights stop summing to one when it is interrupted, or an exit event that
// arrives after the enter event of the next state are all invisible in a screenshot of any one frame.
#include "aver/anim/AnimGraphAsset.hpp"
#include "aver/anim/AnimStateMachine.hpp"
#include "aver/core/Hash.hpp"
#include "aver/core/Log.hpp"

#include <cmath>
#include <map>
#include <string>
#include <vector>

using namespace aver;

static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

static void checkNear(f32 got, f32 want, f32 eps, const std::string& what) {
    if (std::fabs(got - want) <= eps) { AVER_INFO("  ok    {} ({:.5f})", what, got); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {} (got {:.6f}, want {:.6f})", what, got, want);
}

// One bone, one translation track from 0 to `to` over `dur`.
static fmt::OcAnimation slide(f32 to, f32 dur) {
    fmt::OcAnimation c;
    c.duration = dur;
    c.flags = fmt::kOcAnimLoop;
    fmt::OcTrack t;
    t.boneIndex = 0;
    t.channels = fmt::kOcChannelTranslation;
    t.times = {0.0f, dur};
    t.values = {0, 0, 0, to, 0, 0};
    c.tracks.push_back(t);
    return c;
}

static std::map<std::string, fmt::OcAnimation>& clipTable() {
    static std::map<std::string, fmt::OcAnimation> t = {
        {"idle", slide(100.0f, 1.0f)}, {"walk", slide(200.0f, 1.0f)}, {"jump", slide(400.0f, 0.5f)},
        {"fall", slide(800.0f, 1.0f)}, {"run", slide(300.0f, 0.5f)},
    };
    return t;
}

static const fmt::OcAnimation* findClip(const std::string& n) {
    const auto it = clipTable().find(n);
    return it == clipTable().end() ? nullptr : &it->second;
}

static anim::AsmState clipState(const char* name, const char* clip, bool loop = true) {
    anim::AsmState s;
    s.name = name;
    s.asset = clip;
    s.loop = loop;
    return s;
}

static anim::AsmCondition cond(const char* p, anim::AsmOp op, f32 v = 0.0f) { return {p, op, v}; }

static anim::AsmTransition trans(i32 from, i32 to, f32 blend, std::vector<anim::AsmCondition> c = {}) {
    anim::AsmTransition t;
    t.from = from;
    t.to = to;
    t.blendTime = blend;
    t.conditions = std::move(c);
    return t;
}

static fmt::OcSkeleton oneBone() {
    fmt::OcSkeleton s;
    fmt::OcBone b;
    b.name = "root";
    s.bones = {b};
    s.rootBone = 0;
    return s;
}

static void bindIt(anim::AnimStateMachine& m, const anim::AnimStateMachineAsset& a) {
    m.bind(&a, &findClip, [](const std::string&) -> const anim::BlendSpaceAsset* { return nullptr; });
}

static f32 sum(const std::vector<f32>& v) {
    f32 s = 0.0f;
    for (f32 x : v) s += x;
    return s;
}

// Idle <-> Walk on speed, Any -> Jump on a trigger, Jump -> Idle on exit time.
static anim::AnimStateMachineAsset locomotion() {
    anim::AnimStateMachineAsset a;
    a.name = "loco";
    a.params = {{"speed", anim::AsmParamType::Float, 0.0f},
                {"jump", anim::AsmParamType::Trigger, 0.0f},
                {"grounded", anim::AsmParamType::Bool, 1.0f}};
    anim::AsmMachine m;
    m.name = "Root";
    m.states = {clipState("Idle", "idle"), clipState("Walk", "walk"), clipState("Jump", "jump", false)};
    m.states[0].onEnter = "idle_enter";
    m.states[0].onExit = "idle_exit";
    m.states[1].onEnter = "walk_enter";
    m.transitions.push_back(trans(anim::kAsmAny, 2, 0.1f, {cond("jump", anim::AsmOp::Trigger)}));
    m.transitions.push_back(trans(0, 1, 0.5f, {cond("speed", anim::AsmOp::Greater, 0.1f)}));
    m.transitions.push_back(trans(1, 0, 0.5f, {cond("speed", anim::AsmOp::LessEq, 0.1f)}));
    anim::AsmTransition back = trans(2, 0, 0.125f);
    back.hasExitTime = true;
    back.exitTime = 1.0f;
    m.transitions.push_back(back);
    a.machines = {m};
    return a;
}

int main() {
    AVER_INFO("AnimStateMachineTest");

    AVER_INFO("entry, parameters and a timed blend");
    {
        const anim::AnimStateMachineAsset a = locomotion();
        std::string why;
        check(a.valid(&why), "the locomotion asset is valid " + why);
        anim::AnimStateMachine m;
        bindIt(m, a);
        check(m.activeState() == "Idle", "starts in the entry state");

        std::vector<anim::AsmEvent> ev;
        m.drainEvents(ev);
        check(ev.size() == 1 && ev[0].kind == anim::AsmEvent::Kind::Enter && ev[0].name == "idle_enter",
              "binding raises the entry state's enter event");

        for (int i = 0; i < 4; ++i) m.tick(0.125f);
        check(m.activeState() == "Idle" && !m.blending(), "with the condition false nothing happens");

        m.setFloat("speed", 1.0f);
        m.tick(0.125f);
        check(m.activeState() == "Walk", "the transition fires on the tick its condition becomes true");
        check(m.blending(), "and starts blending");
        std::vector<f32> w = m.layerWeights();
        check(w.size() == 2 && w[0] == 1.0f && w[1] == 0.0f, "the new state begins at weight zero");
        ev.clear();
        m.drainEvents(ev);
        check(ev.size() == 2 && ev[0].kind == anim::AsmEvent::Kind::Exit && ev[0].name == "idle_exit" &&
                  ev[1].kind == anim::AsmEvent::Kind::Enter && ev[1].name == "walk_enter",
              "exit of the old state comes before enter of the new one");

        // A 0.5 s blend in 0.125 s steps: 0.25, 0.5, 0.75, then done on the fourth tick.
        m.tick(0.125f);
        w = m.layerWeights();
        checkNear(w[1], 0.25f, 1e-5f, "a quarter of the way through the blend");
        m.tick(0.125f);
        w = m.layerWeights();
        checkNear(w[0], 0.5f, 1e-5f, "half way: half and half");
        checkNear(sum(w), 1.0f, 1e-6f, "weights sum to one mid-blend");
        m.tick(0.125f);
        m.tick(0.125f);
        w = m.layerWeights();
        check(w.size() == 1 && w[0] == 1.0f && !m.blending(), "the blend completes after exactly its blend time");
    }

    AVER_INFO("exit time and a trigger");
    {
        const anim::AnimStateMachineAsset a = locomotion();
        anim::AnimStateMachine m;
        bindIt(m, a);
        m.setTrigger("jump");
        m.tick(0.125f);
        check(m.activeState() == "Jump", "a trigger fires an Any transition");
        check(!m.triggerPending("jump"), "and is consumed");
        for (int i = 0; i < 3; ++i) m.tick(0.125f);
        check(m.activeState() == "Jump", "the 0.5 s jump is still running after 0.375 s");
        m.tick(0.125f);
        check(m.activeState() == "Idle", "exit time 1.0 fires on the tick the clip completes (0.5 s)");
        for (int i = 0; i < 6; ++i) m.tick(0.125f);
        check(m.activeState() == "Idle", "the consumed trigger does not fire again");

        anim::AnimStateMachineAsset b = locomotion();
        b.machines[0].transitions[3].exitTime = 0.5f;
        anim::AnimStateMachine n;
        bindIt(n, b);
        n.setTrigger("jump");
        n.tick(0.125f);
        n.tick(0.125f);
        check(n.activeState() == "Jump", "exit time 0.5 not yet reached after 0.125 s");
        n.tick(0.125f);
        check(n.activeState() == "Idle", "exit time 0.5 fires at 0.25 s");
    }

    AVER_INFO("interruption keeps the weights a partition of one");
    {
        anim::AnimStateMachineAsset a = locomotion();
        a.machines[0].transitions[1].blendTime = 1.0f;
        anim::AnimStateMachine m;
        bindIt(m, a);
        m.setFloat("speed", 1.0f);
        m.tick(0.125f);
        for (int i = 0; i < 2; ++i) m.tick(0.125f);
        check(m.activeState() == "Walk" && m.blending(), "mid-way through Idle -> Walk");
        m.setTrigger("jump");
        m.tick(0.125f);
        check(m.activeState() == "Jump", "a second transition interrupts the first");
        check(m.layerWeights().size() == 3, "three states are live");
        // Stop moving so Jump -> Idle is not followed by another Idle -> Walk blend (speed is still 1).
        m.setFloat("speed", 0.0f);
        bool ok = true;
        for (int i = 0; i < 12; ++i) {
            const std::vector<f32> w = m.layerWeights();
            if (std::fabs(sum(w) - 1.0f) > 1e-5f) ok = false;
            for (f32 x : w) if (x < -1e-6f) ok = false;
            m.tick(0.0625f);
        }
        check(ok, "weights stay non-negative and sum to one through the interruption");
        check(m.layerWeights().size() <= 2, "finished blends are dropped");

        // A non-interruptible transition holds everything else off until its blend completes.
        anim::AnimStateMachineAsset b = locomotion();
        b.machines[0].transitions[1].blendTime = 0.5f;
        b.machines[0].transitions[1].interruptible = false;
        anim::AnimStateMachine n;
        bindIt(n, b);
        n.setFloat("speed", 1.0f);
        n.tick(0.125f);
        n.setTrigger("jump");
        n.tick(0.125f);
        check(n.activeState() == "Walk", "an uninterruptible blend refuses a new transition");
        for (int i = 0; i < 3; ++i) n.tick(0.125f);
        n.tick(0.125f);
        check(n.activeState() == "Jump", "and takes it as soon as the blend has finished");
    }

    AVER_INFO("priority and self transitions");
    {
        anim::AnimStateMachineAsset a;
        a.params = {{"go", anim::AsmParamType::Bool, 1.0f}};
        anim::AsmMachine m;
        m.name = "Root";
        m.states = {clipState("A", "idle"), clipState("B", "walk"), clipState("C", "run")};
        m.transitions.push_back(trans(0, 2, 0.0f, {cond("go", anim::AsmOp::IsTrue)}));
        m.transitions.push_back(trans(0, 1, 0.0f, {cond("go", anim::AsmOp::IsTrue)}));
        m.transitions.push_back(trans(anim::kAsmAny, 2, 0.0f, {cond("go", anim::AsmOp::IsTrue)}));
        a.machines = {m};
        anim::AnimStateMachine s;
        bindIt(s, a);
        s.tick(0.1f);
        check(s.activeState() == "C", "the first listed passing transition wins");
        s.tick(0.1f);
        check(s.activeState() == "C" && s.transitionsFired() == 1, "Any does not re-enter the current state by default");

        a.machines[0].transitions[2].allowSelf = true;
        anim::AnimStateMachine t;
        bindIt(t, a);
        t.tick(0.1f);
        t.tick(0.1f);
        check(t.transitionsFired() == 2, "allowSelf lets Any re-enter the state it is in");
    }

    AVER_INFO("sub-machines");
    {
        anim::AnimStateMachineAsset a;
        a.params = {{"speed", anim::AsmParamType::Float, 0.0f},
                    {"grounded", anim::AsmParamType::Bool, 1.0f},
                    {"land", anim::AsmParamType::Trigger, 0.0f}};
        anim::AsmMachine root;
        root.name = "Root";
        anim::AsmState ground;
        ground.name = "Ground";
        ground.kind = anim::AsmStateKind::SubMachine;
        ground.subMachine = 1;
        ground.onEnter = "ground_enter";
        ground.onExit = "ground_exit";
        root.states = {ground, clipState("Air", "fall")};
        root.states[1].onEnter = "air_enter";
        root.transitions.push_back(trans(0, 1, 0.0f, {cond("grounded", anim::AsmOp::IsFalse)}));
        root.transitions.push_back(trans(1, 0, 0.0f, {cond("grounded", anim::AsmOp::IsTrue)}));

        anim::AsmMachine inner;
        inner.name = "GroundMachine";
        inner.states = {clipState("Idle", "idle"), clipState("Walk", "walk")};
        inner.states[1].onExit = "walk_exit";
        inner.transitions.push_back(trans(0, 1, 0.0f, {cond("speed", anim::AsmOp::Greater, 0.1f)}));
        inner.transitions.push_back(trans(1, 0, 0.0f, {cond("speed", anim::AsmOp::LessEq, 0.1f)}));
        a.machines = {root, inner};
        std::string why;
        check(a.valid(&why), "a root with one sub-machine is valid " + why);

        anim::AnimStateMachine m;
        bindIt(m, a);
        check(m.activePath() == "Ground/Idle", "entering a sub-machine state enters its entry state");
        std::vector<anim::AsmEvent> ev;
        m.drainEvents(ev);
        check(ev.size() == 2 && ev[0].state == "Ground" && ev[1].state == "Idle", "enter events go outermost first");

        m.setFloat("speed", 1.0f);
        m.tick(0.1f);
        check(m.activePath() == "Ground/Walk", "a transition inside the sub-machine");
        ev.clear();
        m.drainEvents(ev);
        check(ev.size() == 2 && ev[0].kind == anim::AsmEvent::Kind::Exit && ev[1].kind == anim::AsmEvent::Kind::Enter,
              "and only the inner states exit and enter, not the sub-machine state");

        m.setBool("grounded", false);
        m.tick(0.1f);
        check(m.activePath() == "Air", "a transition on the sub-machine state leaves from any inner state");
        ev.clear();
        m.drainEvents(ev);
        check(ev.size() == 3 && ev[0].name == "walk_exit" && ev[1].name == "ground_exit" && ev[2].name == "air_enter",
              "exit events go deepest first, then the new state's enter");

        m.setBool("grounded", true);
        m.tick(0.1f);
        check(m.activePath() == "Ground/Idle", "re-entering a sub-machine starts from its entry state again");

        // An inner transition to Exit lets the parent's exit-time transition leave.
        anim::AnimStateMachineAsset b = a;
        b.machines[0].transitions.clear();
        anim::AsmTransition leave = trans(0, 1, 0.0f);
        leave.hasExitTime = true;
        b.machines[0].transitions.push_back(leave);
        b.machines[1].transitions.push_back(trans(1, anim::kAsmExit, 0.0f, {cond("land", anim::AsmOp::Trigger)}));
        check(b.valid(&why), "an Exit transition inside a sub-machine is valid " + why);
        anim::AnimStateMachine n;
        bindIt(n, b);
        n.setFloat("speed", 1.0f);
        n.tick(0.1f);
        check(n.activePath() == "Ground/Walk", "walking");
        n.tick(0.1f);
        check(n.activePath() == "Ground/Walk", "the parent waits while the inner machine has not exited");
        n.setTrigger("land");
        n.tick(0.1f);
        check(n.activePath() == "Ground/Walk", "the exit tick itself only marks the sub-machine finished");
        n.tick(0.1f);
        check(n.activePath() == "Air", "and the parent's exit-time transition takes over on the next");

        anim::AnimStateMachineAsset bad = a;
        bad.machines[1].states[0].kind = anim::AsmStateKind::SubMachine;
        bad.machines[1].states[0].subMachine = 0;
        check(!bad.valid(), "a sub-machine cycle is rejected");
        bad = a;
        bad.machines[0].transitions[0].to = 9;
        check(!bad.valid(), "a transition to a missing state is rejected");
        bad = a;
        bad.machines[0].transitions[0].conditions[0].param = "nope";
        check(!bad.valid(), "a condition on an unknown parameter is rejected");
    }

    AVER_INFO("pose output");
    {
        const anim::AnimStateMachineAsset a = locomotion();
        const fmt::OcSkeleton skel = oneBone();
        anim::AnimStateMachine m;
        bindIt(m, a);
        anim::Pose p;
        m.tick(0.5f);
        m.evaluate(skel, p);
        checkNear(p.local[0].position.x, 50.0f, 1e-3f, "Idle at 0.5 s of a 100 cm slide");

        m.setFloat("speed", 1.0f);
        m.tick(0.0f);
        for (int i = 0; i < 2; ++i) m.tick(0.125f);
        // Idle clock 0.75 s (75), Walk clock 0.125 s (25), weights 0.5 / 0.5 -> hmm, Walk starts when
        // the condition fires (tick 0), so after the two ticks Walk has run 0.25 s and the blend is at
        // 0.25 s of 0.5.
        const std::vector<f32> w = m.layerWeights();
        m.evaluate(skel, p);
        const f32 idleX = 100.0f * (0.5f + 0.0f + 0.25f), walkX = 200.0f * 0.25f;
        checkNear(p.local[0].position.x, w[0] * idleX + w[1] * walkX, 1e-2f, "mid-blend pose is the weighted mix of both live states");
    }

    AVER_INFO("determinism at a fixed step");
    {
        auto run = [&]() {
            const anim::AnimStateMachineAsset a = locomotion();
            anim::AnimStateMachine m;
            bindIt(m, a);
            std::string trace;
            for (int i = 0; i < 80; ++i) {
                if (i == 5) m.setFloat("speed", 1.0f);
                if (i == 30) m.setTrigger("jump");
                if (i == 50) m.setFloat("speed", 0.0f);
                m.tick(1.0f / 60.0f);
                trace += m.activePath() + (m.blending() ? "*" : "") + ";";
            }
            return trace;
        };
        check(run() == run(), "two runs at the same step produce the same state trace");
    }

    AVER_INFO("hash-addressed parameters (what the scene component carries)");
    {
        const anim::AnimStateMachineAsset a = locomotion();
        anim::AnimStateMachine m;
        bindIt(m, a);
        check(m.setByHash(fnv1a64(std::string_view("speed")), 2.5f), "a known hash is accepted");
        checkNear(m.value("speed"), 2.5f, 1e-6f, "and writes the float");
        check(!m.setByHash(fnv1a64(std::string_view("missing")), 1.0f), "an unknown hash is refused");
        m.setByHash(fnv1a64(std::string_view("jump")), 1.0f);
        check(m.triggerPending("jump"), "a non-zero value sets a trigger");
        m.setByHash(fnv1a64(std::string_view("grounded")), 0.0f);
        checkNear(m.value("grounded"), 0.0f, 1e-6f, "a bool takes zero as false");
    }

    AVER_INFO(g_failures == 0 ? "AnimStateMachineTest: PASS" : "AnimStateMachineTest: FAIL");
    return g_failures == 0 ? 0 : 1;
}
