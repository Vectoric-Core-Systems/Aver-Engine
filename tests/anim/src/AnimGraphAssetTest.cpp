// .ocblend and .ocasm: write -> parse -> write is byte-identical, every field survives, and a file
// that is not ours (or is newer than us) is refused instead of misread. No GPU, no scene.
#include "aver/anim/AnimGraphAsset.hpp"
#include "aver/core/Log.hpp"

#include <filesystem>
#include <string>

using namespace aver;

static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

static anim::BlendSpaceAsset makeSpace() {
    anim::BlendSpaceAsset a;
    a.name = "Locomotion \"2D\"";   // quotes exercise the escaper
    a.dims = 2;
    a.axisX = {"direction", -180.0f, 180.0f, 0.15f};
    a.axisY = {"speed", 0.0f, 600.0f, 0.1f};
    a.syncMarkers = true;
    anim::BlendSample idle;
    idle.clip = "Content/Anim/idle.ocanim";
    idle.rate = 1.0f;
    anim::BlendSample walk;
    walk.clip = "Content/Anim/walk.ocanim";
    walk.x = 0.0f;
    walk.y = 150.1f;
    walk.rate = 0.9f;
    walk.markers = {{"L", 0.0333333f}, {"R", 0.5166667f}};
    anim::BlendSample strafe;
    strafe.clip = "Content/Anim/strafe\\r.ocanim";
    strafe.x = 90.0f;
    strafe.y = 300.0f;
    strafe.markers = {{"R", 0.1f}, {"L", 0.6f}};
    a.samples = {idle, walk, strafe};
    return a;
}

static anim::AnimStateMachineAsset makeMachine() {
    anim::AnimStateMachineAsset a;
    a.name = "Hero";
    a.params = {{"speed", anim::AsmParamType::Float, 0.25f},
                {"stance", anim::AsmParamType::Int, 2.0f},
                {"grounded", anim::AsmParamType::Bool, 1.0f},
                {"jump", anim::AsmParamType::Trigger, 0.0f}};
    anim::AsmMachine root;
    root.name = "Root";
    anim::AsmState ground;
    ground.name = "Ground";
    ground.kind = anim::AsmStateKind::SubMachine;
    ground.subMachine = 1;
    ground.posX = 40.5f;
    ground.posY = -12.25f;
    ground.onEnter = "ground_enter";
    anim::AsmState air;
    air.name = "Air";
    air.asset = "Content/Anim/fall.ocanim";
    air.loop = false;
    air.speed = 1.5f;
    air.speedParam = "speed";
    root.states = {ground, air};
    anim::AsmTransition t;
    t.from = 0;
    t.to = 1;
    t.blendTime = 0.15f;
    t.conditions = {{"grounded", anim::AsmOp::IsFalse, 0.0f}, {"speed", anim::AsmOp::GreaterEq, 0.3f}};
    t.interruptible = false;
    root.transitions.push_back(t);
    anim::AsmTransition any;
    any.from = anim::kAsmAny;
    any.to = 0;
    any.hasExitTime = true;
    any.exitTime = 0.875f;
    any.allowSelf = true;
    any.conditions = {{"jump", anim::AsmOp::Trigger, 0.0f}};
    root.transitions.push_back(any);

    anim::AsmMachine inner;
    inner.name = "GroundMachine";
    anim::AsmState loco;
    loco.name = "Loco";
    loco.kind = anim::AsmStateKind::BlendSpace;
    loco.asset = "Content/Anim/loco.ocblend";
    loco.xParam = "stance";
    loco.yParam = "speed";
    loco.onExit = "loco_exit";
    anim::AsmState crouch;
    crouch.name = "Crouch";
    crouch.asset = "Content/Anim/crouch.ocanim";
    inner.states = {loco, crouch};
    inner.entry = 1;
    inner.transitions.push_back({1, anim::kAsmExit, {}, 0.0f, true, 1.0f, true, false});
    a.machines = {root, inner};
    return a;
}

static bool sameSample(const anim::BlendSample& a, const anim::BlendSample& b) {
    if (a.clip != b.clip || a.x != b.x || a.y != b.y || a.rate != b.rate || a.markers.size() != b.markers.size()) return false;
    for (usize i = 0; i < a.markers.size(); ++i)
        if (a.markers[i].name != b.markers[i].name || a.markers[i].time != b.markers[i].time) return false;
    return true;
}

int main() {
    AVER_INFO("AnimGraphAssetTest");

    AVER_INFO("blend space");
    {
        const anim::BlendSpaceAsset a = makeSpace();
        std::string why;
        check(a.valid(&why), "the fixture is valid " + why);
        const std::string text = anim::writeBlendSpace(a);
        anim::BlendSpaceAsset b;
        check(anim::parseBlendSpace(text, b, &why), "it parses back " + why);
        check(b.name == a.name && b.dims == a.dims && b.syncMarkers == a.syncMarkers, "header fields survive");
        check(b.axisX.name == a.axisX.name && b.axisX.min == a.axisX.min && b.axisX.max == a.axisX.max &&
                  b.axisX.smoothing == a.axisX.smoothing && b.axisY.max == a.axisY.max && b.axisY.smoothing == a.axisY.smoothing,
              "axes survive bit for bit");
        bool same = b.samples.size() == a.samples.size();
        for (usize i = 0; same && i < a.samples.size(); ++i) same = sameSample(a.samples[i], b.samples[i]);
        check(same, "every sample, rate and sync marker survives (including awkward floats)");
        check(anim::writeBlendSpace(b) == text, "write -> parse -> write is byte-identical");

        const std::string path = (std::filesystem::temp_directory_path() / "aver-test-loco.ocblend").string();
        check(anim::saveBlendSpace(path, a, &why), "saves to disk " + why);
        anim::BlendSpaceAsset c;
        check(anim::loadBlendSpace(path, c, &why) && anim::writeBlendSpace(c) == text, "loads from disk unchanged");
        std::error_code ec;
        std::filesystem::remove(path, ec);
        check(!anim::loadBlendSpace(path, c), "a missing file is refused");

        anim::BlendSpaceAsset d;
        check(!anim::parseBlendSpace("{\"format\": \"ocasm\", \"version\": 1}", d), "the wrong format tag is refused");
        check(!anim::parseBlendSpace("{\"format\": \"ocblend\", \"version\": 99}", d), "a newer version is refused");
        check(!anim::parseBlendSpace("not json", d), "garbage is refused");
    }

    AVER_INFO("state machine");
    {
        const anim::AnimStateMachineAsset a = makeMachine();
        std::string why;
        check(a.valid(&why), "the fixture is valid " + why);
        const std::string text = anim::writeStateMachine(a);
        anim::AnimStateMachineAsset b;
        check(anim::parseStateMachine(text, b, &why), "it parses back " + why);
        check(b.name == a.name && b.params.size() == 4 && b.machines.size() == 2, "shape survives");
        check(b.params[0].def == 0.25f && b.params[1].type == anim::AsmParamType::Int &&
                  b.params[3].type == anim::AsmParamType::Trigger,
              "parameters and their types survive");
        const anim::AsmState& g = b.machines[0].states[0];
        check(g.kind == anim::AsmStateKind::SubMachine && g.subMachine == 1 && g.posX == 40.5f && g.posY == -12.25f &&
                  g.onEnter == "ground_enter",
              "a sub-machine state survives with its editor position and event");
        const anim::AsmState& air = b.machines[0].states[1];
        check(!air.loop && air.speed == 1.5f && air.speedParam == "speed" && air.asset == "Content/Anim/fall.ocanim",
              "a clip state survives");
        const anim::AsmTransition& t = b.machines[0].transitions[0];
        check(t.conditions.size() == 2 && t.conditions[1].op == anim::AsmOp::GreaterEq && t.conditions[1].value == 0.3f &&
                  t.blendTime == 0.15f && !t.interruptible && !t.hasExitTime,
              "a transition survives with its conditions and flags");
        const anim::AsmTransition& any = b.machines[0].transitions[1];
        check(any.from == anim::kAsmAny && any.hasExitTime && any.exitTime == 0.875f && any.allowSelf &&
                  any.conditions[0].op == anim::AsmOp::Trigger,
              "an Any transition with an exit time survives");
        const anim::AsmMachine& inner = b.machines[1];
        check(inner.entry == 1 && inner.states[0].kind == anim::AsmStateKind::BlendSpace && inner.states[0].xParam == "stance" &&
                  inner.states[0].onExit == "loco_exit" && inner.transitions[0].to == anim::kAsmExit,
              "the inner machine, a blend-space state and an Exit target survive");
        check(anim::writeStateMachine(b) == text, "write -> parse -> write is byte-identical");

        const std::string path = (std::filesystem::temp_directory_path() / "aver-test-hero.ocasm").string();
        anim::AnimStateMachineAsset c;
        check(anim::saveStateMachine(path, a, &why) && anim::loadStateMachine(path, c, &why) &&
                  anim::writeStateMachine(c) == text,
              "a file round trip is unchanged " + why);
        std::error_code ec;
        std::filesystem::remove(path, ec);

        anim::AnimStateMachineAsset d;
        check(!anim::parseStateMachine("{\"format\": \"ocblend\", \"version\": 1}", d), "the wrong format tag is refused");
        check(!anim::parseStateMachine(
                  "{\"format\": \"ocasm\", \"version\": 1, \"machines\": [{\"name\": \"R\", \"states\": [], \"transitions\": "
                  "[{\"from\": 0, \"to\": 0, \"when\": [{\"param\": \"x\", \"op\": \"bogus\", \"value\": 0}]}]}]}",
                  d),
              "an unknown condition op is refused");

        // Half-authored files still round trip; the runtime is what validates.
        anim::AnimStateMachineAsset e = a;
        e.machines[0].transitions[0].to = 7;
        anim::AnimStateMachineAsset f;
        check(anim::parseStateMachine(anim::writeStateMachine(e), f) && !f.valid(), "an invalid asset round trips and is still flagged invalid");
    }

    AVER_INFO(g_failures == 0 ? "AnimGraphAssetTest: PASS" : "AnimGraphAssetTest: FAIL");
    return g_failures == 0 ? 0 : 1;
}
