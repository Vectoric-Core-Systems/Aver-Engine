// The sparse curve channel on a Pose (blend-shape weights and face-pose weights): how the samplers fill
// it, the two blend rules, the masked and additive forms, the cap, and that it survives every node of the
// .ocasm pipeline and a state-machine / player crossfade. No scene and no GPU.
//
// The failure worth catching is the silent one: a blend or swap that leaves the curves behind, so a face
// stops animating after a slot or layered node and nothing else looks wrong.
#include "aver/anim/AnimGraphAsset.hpp"
#include "aver/anim/AnimPipeline.hpp"
#include "aver/anim/AnimSampler.hpp"
#include "aver/anim/AnimStateMachine.hpp"
#include "aver/core/Hash.hpp"
#include "aver/core/Log.hpp"

#include <cmath>
#include <initializer_list>
#include <limits>
#include <map>
#include <string>
#include <utility>
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

static u64 idOf(const char* name) { return fnv1a64(std::string_view(name)); }

static fmt::OcSkeleton rig() {
    fmt::OcSkeleton s;
    auto bone = [](const char* n, i32 parent) {
        fmt::OcBone b;
        b.name = n;
        b.parent = parent;
        return b;
    };
    s.bones = {bone("root", -1), bone("spine", 0), bone("head", 1), bone("hand", 2), bone("leg", 0)};
    s.rootBone = 0;
    return s;
}

// A curve value in a pose, or NaN when absent.
static f32 morphOf(const anim::Pose& p, const char* n) {
    const anim::PoseCurve* c = anim::findPoseCurve(p, anim::kPoseCurveMorph, idOf(n));
    return c ? c->value : std::numeric_limits<f32>::quiet_NaN();
}
static f32 faceOf(const anim::Pose& p, const char* n) {
    const anim::PoseCurve* c = anim::findPoseCurve(p, anim::kPoseCurveFace, idOf(n));
    return c ? c->value : std::numeric_limits<f32>::quiet_NaN();
}
static bool absent(f32 v) { return std::isnan(v); }

static anim::Pose withCurves(const fmt::OcSkeleton& s, std::initializer_list<std::pair<const char*, f32>> morph) {
    anim::Pose p;
    anim::restPose(s, p);
    for (const auto& m : morph) anim::setPoseCurve(p, anim::kPoseCurveMorph, idOf(m.first), m.second);
    return p;
}

static fmt::OcCurve curve(const char* name, std::vector<f32> t, std::vector<f32> v) {
    fmt::OcCurve c;
    c.name = name;
    c.times = std::move(t);
    c.values = std::move(v);
    return c;
}

static std::map<std::string, fmt::OcAnimation>& clips() {
    static std::map<std::string, fmt::OcAnimation> t;
    return t;
}
static const fmt::OcAnimation* findClip(const std::string& n) {
    const auto it = clips().find(n);
    return it == clips().end() ? nullptr : &it->second;
}

static fmt::OcAnimation slide(f32 dur, f32 from, f32 to, std::vector<u16> bones) {
    fmt::OcAnimation c;
    c.duration = dur;
    c.flags = fmt::kOcAnimLoop;
    for (const u16 b : bones) {
        fmt::OcTrack t;
        t.boneIndex = b;
        t.channels = fmt::kOcChannelTranslation;
        t.times = {0.0f, dur};
        t.values = {from, 0, 0, to, 0, 0};
        c.tracks.push_back(t);
    }
    return c;
}

static const anim::AsmBlendSpaceFn kNoSpaces = [](const std::string&) -> const anim::BlendSpaceAsset* { return nullptr; };
static const anim::ParamFn kNoParams = [](const std::string&) { return std::numeric_limits<f32>::quiet_NaN(); };
static const anim::SlotEvalFn kNoSlots = [](u64, const anim::Pose&, anim::Pose&, f32&) { return false; };

static void testRestAndSampling() {
    AVER_INFO("=== restPose clears; samplers fill only prefixed curves ===");
    const fmt::OcSkeleton skel = rig();
    anim::Pose p = withCurves(skel, {{"jawOpen", 1.0f}});
    check(p.curves.size() == 1, "a curve was set");
    anim::restPose(skel, p);
    check(p.curves.empty(), "restPose clears the curves so stale weights cannot leak between frames");

    fmt::OcAnimation clip = slide(1.0f, 0.0f, 10.0f, {1});
    clip.curves.push_back(curve("morph.jawOpen", {0.0f, 1.0f}, {0.0f, 1.0f}));
    clip.curves.push_back(curve("pose.smile", {0.0f, 1.0f}, {0.5f, 0.5f}));
    clip.curves.push_back(curve("Footstep", {0.0f, 1.0f}, {7.0f, 7.0f}));   // an ordinary curve
    anim::sampleAnimation(clip, 0.5f, p);
    check(p.curves.size() == 2, "only the morph. and pose. curves enter the pose");
    checkNear(morphOf(p, "jawOpen"), 0.5f, 1e-5f, "a morph curve at t = 0.5");
    checkNear(faceOf(p, "smile"), 0.5f, 1e-5f, "a pose curve");
    check(absent(morphOf(p, "Footstep")) && absent(faceOf(p, "Footstep")), "an ordinary curve is untouched");
    check(absent(faceOf(p, "jawOpen")), "kinds are distinct: jawOpen is not a face pose");
    check(clip.curves[0].poseId == idOf("jawOpen"), "the sampler caches the hash of the bare name in poseId");
    check(clip.curves[2].poseId == 0, "and leaves an ordinary curve alone");
    check(p.curves[0].kind <= p.curves[1].kind, "curves are sorted by (kind, id)");

    // The skeleton-less path: no bones at all.
    anim::Pose bare;
    anim::sampleClipCurves(clip, 1.0f, bare);
    check(bare.local.empty() && bare.curves.size() == 2, "sampleClipCurves needs no skeleton");
    checkNear(morphOf(bare, "jawOpen"), 1.0f, 1e-5f, "and samples at the given time");
}

static void testBlend() {
    AVER_INFO("=== blendPose: crossfade and overlay ===");
    const fmt::OcSkeleton skel = rig();
    const anim::Pose a = withCurves(skel, {{"x", 1.0f}, {"both", 0.0f}});
    const anim::Pose b = withCurves(skel, {{"y", 1.0f}, {"both", 1.0f}});
    anim::Pose out;
    anim::blendPose(a, b, 0.25f, out);
    checkNear(morphOf(out, "x"), 0.75f, 1e-6f, "crossfade: a curve only in a fades by 1 - t");
    checkNear(morphOf(out, "y"), 0.25f, 1e-6f, "crossfade: a curve only in b rises by t");
    checkNear(morphOf(out, "both"), 0.25f, 1e-6f, "a curve in both lerps");
    anim::blendPose(a, b, 0.25f, out, anim::CurveBlend::Overlay);
    checkNear(morphOf(out, "x"), 1.0f, 1e-6f, "overlay: a curve only in a is kept");
    checkNear(morphOf(out, "y"), 0.25f, 1e-6f, "overlay: a curve only in b becomes b * t");
    checkNear(morphOf(out, "both"), 0.25f, 1e-6f, "overlay: both lerp");

    anim::blendPose(a, b, 0.0f, out);
    checkNear(morphOf(out, "x"), 1.0f, 1e-6f, "t = 0 keeps a");
    checkNear(morphOf(out, "y"), 0.0f, 1e-6f, "and b contributes nothing");
    anim::blendPose(a, b, 1.0f, out);
    checkNear(morphOf(out, "x"), 0.0f, 1e-6f, "t = 1: a's own curve is gone");
    checkNear(morphOf(out, "y"), 1.0f, 1e-6f, "b is whole");

    anim::Pose small;
    small.local.resize(2);
    anim::setPoseCurve(small, anim::kPoseCurveMorph, idOf("z"), 1.0f);
    anim::blendPose(a, small, 0.5f, out);
    check(out.local.size() == a.local.size() && out.curves.size() == a.curves.size() && morphOf(out, "x") == 1.0f &&
          absent(morphOf(out, "z")), "a size mismatch returns a whole, curves included");

    // out may alias a
    anim::Pose c = withCurves(skel, {{"x", 1.0f}});
    anim::blendPose(c, b, 0.5f, c);
    checkNear(morphOf(c, "x"), 0.5f, 1e-6f, "out may alias a");
}

static void testMaskedAndAdditive() {
    AVER_INFO("=== blendPoseMasked and addPose ===");
    const fmt::OcSkeleton skel = rig();
    const anim::Pose a = withCurves(skel, {{"x", 1.0f}, {"both", 0.0f}});
    const anim::Pose b = withCurves(skel, {{"y", 1.0f}, {"both", 1.0f}});
    anim::Pose out;
    anim::blendPoseMasked(a, b, {0.0f, 0.3f, 0.6f, 0.0f, 0.1f}, out);
    checkNear(morphOf(out, "both"), 0.6f, 1e-6f, "the curve weight is the largest bone weight");
    checkNear(morphOf(out, "y"), 0.6f, 1e-6f, "a curve only in b becomes b * that weight");
    checkNear(morphOf(out, "x"), 1.0f, 1e-6f, "a curve only in a is kept (overlay)");

    const anim::Pose none = withCurves(skel, {});
    anim::blendPoseMasked(a, none, {1.0f, 1.0f, 1.0f, 1.0f, 1.0f}, out);
    checkNear(morphOf(out, "x"), 1.0f, 1e-6f, "a layer with no curves leaves the base's face alone");
    anim::blendPoseMasked(a, b, {1.0f}, out);
    check(morphOf(out, "x") == 1.0f && absent(morphOf(out, "y")), "a bad mask size returns a, curves included");

    const anim::Pose base = withCurves(skel, {{"x", 0.2f}});
    const anim::Pose add = withCurves(skel, {{"x", 0.9f}, {"z", 0.5f}});
    const anim::Pose rest = withCurves(skel, {{"x", 0.4f}});
    anim::addPose(base, add, rest, 0.5f, out);
    checkNear(morphOf(out, "x"), 0.2f + (0.9f - 0.4f) * 0.5f, 1e-6f, "addPose: base + (additive - rest) * w");
    checkNear(morphOf(out, "z"), 0.25f, 1e-6f, "a curve the base lacks and the rest lacks adds from 0");
    anim::addPose(base, none, rest, 1.0f, out);
    checkNear(morphOf(out, "x"), 0.2f, 1e-6f, "an additive without curves leaves the base's");
}

static void testCap() {
    AVER_INFO("=== the 256-curve cap and ordering ===");
    anim::Pose p;
    for (u64 i = 0; i < 300; ++i) anim::setPoseCurve(p, anim::kPoseCurveMorph, 1000 - i, f32(i));
    check(p.curves.size() == anim::kMaxPoseCurves, "capped at 256");
    bool sorted = true;
    for (usize i = 1; i < p.curves.size(); ++i)
        if (p.curves[i - 1].id >= p.curves[i].id) sorted = false;
    check(sorted, "kept sorted by id however they were inserted");
    const u64 someId = p.curves[10].id;
    anim::setPoseCurve(p, anim::kPoseCurveMorph, someId, 99.0f);
    check(p.curves.size() == anim::kMaxPoseCurves && anim::findPoseCurve(p, anim::kPoseCurveMorph, someId)->value == 99.0f,
          "at the cap an existing curve is still overwritten");
    anim::setPoseCurve(p, anim::kPoseCurveFace, 5, 1.0f);
    check(p.curves.size() == anim::kMaxPoseCurves && !anim::findPoseCurve(p, anim::kPoseCurveFace, 5), "and a new one is dropped");

    anim::Pose q;
    anim::setPoseCurve(q, anim::kPoseCurveFace, 2, 1.0f);
    anim::setPoseCurve(q, anim::kPoseCurveMorph, 9, 1.0f);
    check(q.curves[0].kind == anim::kPoseCurveMorph && q.curves[1].kind == anim::kPoseCurveFace, "sorted by kind first");
}

static anim::PipeNode curveNode(const char* curve, f32 value, bool add, const char* valueParam = "", f32 w = 1.0f) {
    anim::PipeNode n;
    n.kind = anim::PipeNodeKind::Curve;
    n.curve = curve;
    n.value = value;
    n.curveAdd = add;
    n.valueParam = valueParam;
    n.weight = w;
    return n;
}

static void testPipeline() {
    AVER_INFO("=== curves survive the pipeline: slot, layered, additive (the swap sites) ===");
    const fmt::OcSkeleton skel = rig();
    fmt::OcAnimation upper = slide(1.0f, 0.0f, 100.0f, {1});
    upper.curves.push_back(curve("morph.smile", {0.0f, 1.0f}, {1.0f, 1.0f}));
    clips()["upper"] = upper;
    clips()["add"] = slide(1.0f, 2.0f, 12.0f, {1});

    {   // a slot node whose montage pose has no curves must not fade the base's face (overlay)
        anim::PipeNode n;
        n.kind = anim::PipeNodeKind::Slot;
        n.slot = "Full";
        std::vector<anim::PipeNode> nodes = {n};
        anim::AnimPipeline pipe;
        pipe.bind(&nodes, &skel, &findClip, kNoSpaces);
        anim::Pose p = withCurves(skel, {{"jawOpen", 1.0f}});
        const anim::SlotEvalFn slot = [&](u64, const anim::Pose&, anim::Pose& out, f32& cov) {
            anim::restPose(skel, out);
            out.local[1].position.x = 50.0f;
            cov = 0.5f;
            return true;
        };
        pipe.evaluate(kNoParams, slot, p);
        checkNear(p.local[1].position.x, 25.0f, 1e-4f, "the slot blends the bones");
        checkNear(morphOf(p, "jawOpen"), 1.0f, 1e-6f, "and keeps the base's curve at full (overlay, not a fade to 0.5)");
        anim::Pose q = withCurves(skel, {{"jawOpen", 1.0f}});
        const anim::SlotEvalFn withFace = [&](u64, const anim::Pose& base, anim::Pose& out, f32& cov) {
            out = base;
            anim::setPoseCurve(out, anim::kPoseCurveMorph, idOf("jawOpen"), 0.0f);
            cov = 0.5f;
            return true;
        };
        pipe.evaluate(kNoParams, withFace, q);
        checkNear(morphOf(q, "jawOpen"), 0.5f, 1e-6f, "a slot that does drive the curve lerps it");
    }
    {   // layered
        anim::PipeNode n;
        n.kind = anim::PipeNodeKind::Layered;
        n.source.kind = anim::PipeSourceKind::Clip;
        n.source.asset = "upper";
        n.branches = {{"spine", 0, 1.0f}};
        std::vector<anim::PipeNode> nodes = {n};
        anim::AnimPipeline pipe;
        pipe.bind(&nodes, &skel, &findClip, kNoSpaces);
        pipe.tick(0.5f, kNoParams);
        anim::Pose p = withCurves(skel, {{"jawOpen", 1.0f}});
        pipe.evaluate(kNoParams, kNoSlots, p);
        checkNear(p.local[1].position.x, 50.0f, 1e-3f, "the layered clip moves its branch");
        checkNear(morphOf(p, "jawOpen"), 1.0f, 1e-6f, "the base's curve survives the layered swap");
        checkNear(morphOf(p, "smile"), 1.0f, 1e-6f, "the layered clip's curve arrives, weighted by the branch");
    }
    {   // additive
        anim::PipeNode n;
        n.kind = anim::PipeNodeKind::Additive;
        n.source.kind = anim::PipeSourceKind::Clip;
        n.source.asset = "add";
        std::vector<anim::PipeNode> nodes = {n};
        anim::AnimPipeline pipe;
        pipe.bind(&nodes, &skel, &findClip, kNoSpaces);
        pipe.tick(0.5f, kNoParams);
        anim::Pose p = withCurves(skel, {{"jawOpen", 0.75f}});
        pipe.evaluate(kNoParams, kNoSlots, p);
        checkNear(p.local[1].position.x, 5.0f, 1e-3f, "the additive clip adds to its bone");
        checkNear(morphOf(p, "jawOpen"), 0.75f, 1e-6f, "the base's curve survives the additive swap");
    }
    {   // a slot-sourced additive node
        anim::PipeNode n;
        n.kind = anim::PipeNodeKind::Additive;
        n.source.kind = anim::PipeSourceKind::Slot;
        n.source.slot = "Add";
        std::vector<anim::PipeNode> nodes = {n};
        anim::AnimPipeline pipe;
        pipe.bind(&nodes, &skel, &findClip, kNoSpaces);
        anim::Pose p = withCurves(skel, {{"jawOpen", 1.0f}});
        const anim::SlotEvalFn slot = [&](u64, const anim::Pose&, anim::Pose& out, f32& cov) {
            anim::restPose(skel, out);
            cov = 1.0f;
            return true;
        };
        pipe.evaluate(kNoParams, slot, p);
        checkNear(morphOf(p, "jawOpen"), 1.0f, 1e-6f, "a slot-sourced additive keeps the base's curve (overlay)");
    }
}

static void testCurveNode() {
    AVER_INFO("=== the .ocasm curve node ===");
    const fmt::OcSkeleton skel = rig();
    anim::Pose base = withCurves(skel, {{"browInnerUp", 0.5f}});
    const auto param = [](f32 v) {
        return anim::ParamFn([v](const std::string& n) { return n == "Anger" ? v : std::numeric_limits<f32>::quiet_NaN(); });
    };
    {   // set with a constant, full weight
        std::vector<anim::PipeNode> nodes = {curveNode("morph.browInnerUp", 0.2f, false), curveNode("pose.smile", 0.8f, false)};
        anim::AnimPipeline pipe;
        pipe.bind(&nodes, &skel, &findClip, kNoSpaces);
        pipe.tick(0.1f, kNoParams);
        anim::Pose p = base;
        pipe.evaluate(kNoParams, kNoSlots, p);
        checkNear(morphOf(p, "browInnerUp"), 0.2f, 1e-6f, "set at weight 1 replaces the value");
        checkNear(faceOf(p, "smile"), 0.8f, 1e-6f, "a pose. curve is a face-pose weight");
    }
    {   // set at weight 0.25 lerps from the existing value (or 0)
        std::vector<anim::PipeNode> nodes = {curveNode("morph.browInnerUp", 1.0f, false, "", 0.25f), curveNode("morph.other", 1.0f, false, "", 0.25f)};
        anim::AnimPipeline pipe;
        pipe.bind(&nodes, &skel, &findClip, kNoSpaces);
        anim::Pose p = base;
        pipe.evaluate(kNoParams, kNoSlots, p);
        checkNear(morphOf(p, "browInnerUp"), 0.5f + (1.0f - 0.5f) * 0.25f, 1e-6f, "set lerps from the existing value");
        checkNear(morphOf(p, "other"), 0.25f, 1e-6f, "and from 0 when there is none");
    }
    {   // add, param-driven
        std::vector<anim::PipeNode> nodes = {curveNode("morph.browInnerUp", 0.0f, true, "Anger")};
        anim::AnimPipeline pipe;
        pipe.bind(&nodes, &skel, &findClip, kNoSpaces);
        anim::Pose p = base;
        pipe.evaluate(param(0.3f), kNoSlots, p);
        checkNear(morphOf(p, "browInnerUp"), 0.8f, 1e-6f, "add: curve += parameter * weight");
        p = base;
        pipe.evaluate(kNoParams, kNoSlots, p);
        checkNear(morphOf(p, "browInnerUp"), 0.5f, 1e-6f, "a missing parameter falls back to the constant (0)");
    }
    {   // asset: round trip, validation, unknown type
        anim::AnimStateMachineAsset a;
        a.name = "Face";
        anim::AsmMachine m;
        m.name = "Root";
        anim::AsmState s;
        s.name = "Idle";
        s.asset = "idle";
        m.states = {s};
        a.machines = {m};
        a.params = {{"Anger", anim::AsmParamType::Float, 0.0f}};
        anim::PipeNode n = curveNode("morph.browDownLeft", 0.1f, true, "Anger", 0.75f);
        n.weightParam = "Anger";
        a.pipeline = {n, curveNode("pose.smile", 1.0f, false)};
        std::string why;
        check(a.valid(&why), "a curve pipeline validates: " + why);
        const std::string text = anim::writeStateMachine(a);
        anim::AnimStateMachineAsset b;
        check(anim::parseStateMachine(text, b, &why), "writes and parses: " + why);
        check(b.pipeline.size() == 2 && b.pipeline[0].kind == anim::PipeNodeKind::Curve, "the nodes survive");
        if (b.pipeline.size() == 2) {
            const anim::PipeNode& r = b.pipeline[0];
            check(r.curve == "morph.browDownLeft" && r.valueParam == "Anger" && r.curveAdd && r.weightParam == "Anger",
                  "curve, valueParam, mode and weightParam round-trip");
            checkNear(r.value, 0.1f, 1e-6f, "value");
            checkNear(r.weight, 0.75f, 1e-6f, "weight");
            check(!b.pipeline[1].curveAdd, "mode set is the default");
        }
        anim::AnimStateMachineAsset bad = a;
        bad.pipeline = {curveNode("jawOpen", 0.0f, false)};
        check(!bad.valid(&why), "a curve name without morph. or pose. is invalid: " + why);
        std::string broken = text;
        const usize at = broken.find("\"curve\"");
        check(at != std::string::npos, "the file names the node type");
        const usize t = broken.find("\"type\": \"curve\"");
        if (t != std::string::npos) broken.replace(t, 15, "\"type\": \"nope\"");
        anim::AnimStateMachineAsset c;
        check(!anim::parseStateMachine(broken, c, &why), "an unknown node type still fails: " + why);
    }
}

static void testMachineAndPlayer() {
    AVER_INFO("=== crossfades carry curves: state machine and player ===");
    fmt::OcSkeleton one;
    {
        fmt::OcBone b;
        b.name = "root";
        one.bones = {b};
        one.rootBone = 0;
    }
    fmt::OcAnimation face = slide(1.0f, 0.0f, 10.0f, {0});
    face.curves.push_back(curve("morph.jawOpen", {0.0f, 1.0f}, {1.0f, 1.0f}));
    fmt::OcAnimation plain = slide(1.0f, 0.0f, 20.0f, {0});
    clips()["face"] = face;
    clips()["plain"] = plain;

    {
        anim::AnimStateMachineAsset a;
        a.name = "M";
        a.params = {{"go", anim::AsmParamType::Trigger, 0.0f}};
        anim::AsmMachine m;
        m.name = "Root";
        anim::AsmState s0, s1;
        s0.name = "Face"; s0.asset = "face";
        s1.name = "Plain"; s1.asset = "plain";
        m.states = {s0, s1};
        anim::AsmTransition t;
        t.from = 0; t.to = 1; t.blendTime = 1.0f;
        t.conditions = {{"go", anim::AsmOp::Trigger, 0.0f}};
        m.transitions = {t};
        a.machines = {m};
        anim::AnimStateMachine sm;
        sm.bind(&a, &findClip, kNoSpaces);
        anim::Pose p;
        sm.tick(0.1f);
        sm.evaluate(one, p);
        checkNear(morphOf(p, "jawOpen"), 1.0f, 1e-6f, "the first state's curve plays");
        sm.setTrigger("go");
        sm.tick(0.05f);
        sm.tick(0.4f);
        const std::vector<f32> w = sm.layerWeights();
        check(w.size() == 2 && w[0] > 0.0f && w[0] < 1.0f, "mid-blend between the two states");
        sm.evaluate(one, p);
        if (w.size() == 2) checkNear(morphOf(p, "jawOpen"), w[0], 1e-4f,
                                     "a state without face curves fades the face out (crossfade): the curve follows the layer weight");
    }
    {
        anim::AnimPlayer pl;
        pl.play(findClip("face"), 0.0f);
        pl.advance(0.1f);
        pl.play(findClip("plain"), 0.5f);
        pl.advance(0.25f);
        anim::Pose p;
        pl.evaluate(one, p);
        checkNear(morphOf(p, "jawOpen"), 0.5f, 1e-4f, "an AnimPlayer crossfade fades the outgoing clip's curve");
    }
}

int main() {
    testRestAndSampling();
    testBlend();
    testMaskedAndAdditive();
    testCap();
    testPipeline();
    testCurveNode();
    testMachineAndPlayer();
    if (g_failures == 0) AVER_INFO("=== all pose curve tests passed ===");
    else                 AVER_ERROR("=== {} FAILED ===", g_failures);
    return g_failures == 0 ? 0 : 1;
}
