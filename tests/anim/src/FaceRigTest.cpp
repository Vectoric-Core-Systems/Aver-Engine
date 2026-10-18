// The .ocface asset, the built-in ARKit/viseme rig, pose resolution and joint poses on a parented rig.
// No scene and no GPU.
#include "aver/anim/FaceRig.hpp"
#include "aver/core/Hash.hpp"
#include "aver/core/Log.hpp"

#include <cmath>
#include <functional>
#include <set>
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

static u64 idOf(std::string_view n) { return fnv1a64(n); }

static anim::FaceRigAsset sampleRig() {
    anim::FaceRigAsset a;
    a.convention = "custom";
    a.mesh = "Characters/Hero_Head.ocmesh";
    a.skeleton = "Characters/Hero.ocskel";
    anim::FacePose smile;
    smile.name = "smile";
    smile.targets = {{"mouthSmileLeft", 0.9f}, {"mouthSmileRight", 1.0f / 3.0f}};
    anim::FaceJoint j;
    j.bone = "jaw";
    j.position = Vec3{0.1f, 0.2f, -0.3f};
    j.rotation = Quat{0.1f, 0.2f, 0.3f, 0.9f};
    j.scale = Vec3{1.0f, 1.25f, 1.0f};
    smile.joints = {j};
    anim::FacePose frown;
    frown.name = "frown";
    frown.targets = {{"mouthFrownLeft", 1.0f}};
    a.poses = {smile, frown};
    a.phonemes = {{"AA", "viseme_oh"}, {"XX", "viseme_PP"}};
    a.lipsync.lead = 0.03f;
    a.lipsync.maxTotal = 0.8f;
    return a;
}

static void testAsset() {
    AVER_INFO("=== .ocface round trip and validation ===");
    const anim::FaceRigAsset a = sampleRig();
    std::string why;
    check(a.valid(&why), "the sample rig is valid: " + why);
    const std::string text = anim::writeFaceRig(a);
    anim::FaceRigAsset b;
    check(anim::parseFaceRig(text, b, &why), "writes and parses: " + why);
    check(b.convention == "custom" && b.mesh == a.mesh && b.skeleton == a.skeleton, "header fields");
    check(b.poses.size() == 2 && b.poses[0].name == "smile" && b.poses[1].name == "frown", "poses and their order");
    if (b.poses.size() == 2) {
        check(b.poses[0].targets.size() == 2 && b.poses[0].targets[1].weight == 1.0f / 3.0f, "nine significant digits are lossless for f32");
        check(b.poses[0].joints.size() == 1 && b.poses[0].joints[0].bone == "jaw", "joints");
        const anim::FaceJoint& j = b.poses[0].joints[0];
        check(j.position.z == -0.3f && j.rotation.w == 0.9f && j.scale.y == 1.25f, "joint position, rotation and scale survive exactly");
    }
    check(b.phonemes.size() == 2 && b.phonemes[0].first == "AA" && b.phonemes[0].second == "viseme_oh", "phoneme overrides");
    check(b.lipsync.lead == 0.03f && b.lipsync.maxTotal == 0.8f && b.lipsync.release == 0.08f, "lip-sync parameters (unwritten ones keep their defaults)");

    // unknown keys are ignored
    std::string extra = text;
    extra.insert(extra.find('{') + 1, "\"futureThing\": {\"a\": [1, 2, 3]}, ");
    anim::FaceRigAsset c;
    check(anim::parseFaceRig(extra, c, &why) && c.poses.size() == 2, "unknown keys are ignored: " + why);

    // parse does not validate (a half-authored rig round-trips)
    anim::FaceRigAsset half = a;
    half.poses[0].name.clear();
    anim::FaceRigAsset d;
    check(anim::parseFaceRig(anim::writeFaceRig(half), d, &why) && d.poses[0].name.empty(), "parse does not call valid()");

    // header gating
    check(!anim::parseFaceRig("{\"format\": \"ocikrig\", \"version\": 1}", d, &why), "another asset's tag is refused: " + why);
    check(!anim::parseFaceRig("{\"format\": \"ocface\", \"version\": 99}", d, &why), "a newer version is refused: " + why);
    check(!anim::parseFaceRig("not json", d, &why), "garbage is refused: " + why);

    // every invalid shape names itself
    const auto bad = [&](const char* label, const std::function<void(anim::FaceRigAsset&)>& mutate) {
        anim::FaceRigAsset x = sampleRig();
        mutate(x);
        std::string w;
        check(!x.valid(&w) && !w.empty(), std::string(label) + ": " + w);
    };
    bad("257 poses", [](anim::FaceRigAsset& x) { x.poses.assign(257, anim::FacePose{}); for (usize i = 0; i < x.poses.size(); ++i) x.poses[i].name = "p" + std::to_string(i); });
    bad("duplicate pose names", [](anim::FaceRigAsset& x) { x.poses[1].name = x.poses[0].name; });
    bad("an empty pose name", [](anim::FaceRigAsset& x) { x.poses[0].name.clear(); });
    bad("a pose name over 128 bytes", [](anim::FaceRigAsset& x) { x.poses[0].name.assign(129, 'a'); });
    bad("33 targets", [](anim::FaceRigAsset& x) { x.poses[0].targets.assign(33, anim::FaceTargetRef{"t", 1.0f}); });
    bad("17 joints", [](anim::FaceRigAsset& x) { x.poses[0].joints.assign(17, anim::FaceJoint{"jaw"}); });
    bad("a weight of 5", [](anim::FaceRigAsset& x) { x.poses[0].targets[0].weight = 5.0f; });
    bad("a NaN weight", [](anim::FaceRigAsset& x) { x.poses[0].targets[0].weight = std::nanf(""); });
    bad("an empty target name", [](anim::FaceRigAsset& x) { x.poses[0].targets[0].name.clear(); });
    bad("a joint with no bone", [](anim::FaceRigAsset& x) { x.poses[0].joints[0].bone.clear(); });
    bad("a non-finite joint value", [](anim::FaceRigAsset& x) { x.poses[0].joints[0].position.x = INFINITY; });
    bad("a negative lip-sync parameter", [](anim::FaceRigAsset& x) { x.lipsync.release = -1.0f; });
    bad("an empty phoneme mapping", [](anim::FaceRigAsset& x) { x.phonemes[0].second.clear(); });
}

static void testDefaults() {
    AVER_INFO("=== the built-in rig ===");
    const anim::FaceRigAsset& rig = anim::defaultFaceRig();
    std::string why;
    check(rig.valid(&why), "it is a valid rig: " + why);
    std::set<std::string> arkit;
    for (u32 i = 0; i < anim::kArkitShapeCount; ++i) arkit.insert(anim::kArkitShapes[i]);
    check(arkit.size() == 52, "52 distinct ARKit names");
    bool all = true;
    for (const std::string& n : arkit) if (!rig.find(n)) all = false;
    check(all, "every ARKit name is a pose");
    bool visemes = true;
    for (u32 i = 0; i < anim::kVisemeCount; ++i) if (!rig.find(anim::kVisemeNames[i])) visemes = false;
    check(visemes, "all 15 viseme poses exist");
    bool targetsOk = true;
    for (u32 i = 0; i < anim::kVisemeCount; ++i) {
        const anim::FacePose* p = rig.find(anim::kVisemeNames[i]);
        if (!p) continue;
        for (const anim::FaceTargetRef& t : p->targets) if (!arkit.count(t.name)) targetsOk = false;
    }
    check(targetsOk, "every viseme target is an ARKit name");
    check(rig.find("viseme_sil") && rig.find("viseme_sil")->targets.empty(), "viseme_sil moves nothing");
    check(rig.find("viseme_aa") && rig.find("viseme_aa")->targets.size() == 3, "viseme_aa is jawOpen plus both lower-lip shapes");

    u32 count = 0;
    bool resolves = true;
    for (u32 i = 0; anim::arpabetSymbol(i); ++i) {
        ++count;
        const char* v = anim::arpabetViseme(anim::arpabetSymbol(i));
        if (!v || !rig.find(v)) resolves = false;
    }
    check(count == 39, "the ARPAbet table has 39 symbols");
    check(resolves, "each resolves to a viseme pose");
    check(std::string(anim::arpabetViseme("sil")) == "viseme_sil" && std::string(anim::arpabetViseme("sp")) == "viseme_sil" &&
          std::string(anim::arpabetViseme("pau")) == "viseme_sil", "sil, sp and pau are silence");
    check(anim::arpabetViseme("QQ") == nullptr, "an unknown symbol is null");

    check(anim::resolveVisemeName(nullptr, "HH") == "viseme_ih", "a phoneme resolves through ARPAbet");
    check(anim::resolveVisemeName(nullptr, "hh") == "viseme_ih", "case does not matter for phonemes");
    check(anim::resolveVisemeName(nullptr, "aa") == "viseme_aa" || anim::resolveVisemeName(nullptr, "aa") == "viseme_aa",
          "a viseme short name is accepted");
    check(anim::resolveVisemeName(nullptr, "viseme_PP") == "viseme_PP", "and a full one");
    check(anim::resolveVisemeName(nullptr, "nonsense").empty(), "unknown is empty");
    const anim::FaceRigAsset custom = sampleRig();
    check(anim::resolveVisemeName(&custom, "AA") == "viseme_oh", "a rig override beats the table");
    check(anim::resolveVisemeName(&custom, "XX") == "viseme_PP", "and adds symbols");
    check(anim::resolveVisemeName(&custom, "AA", true) == "viseme_aa", "visemeOnly skips overrides and ARPAbet (AA is the viseme aa)");
}

static void testResolution() {
    AVER_INFO("=== pose resolution: explicit, mesh target, built-in ===");
    const anim::FaceRigAsset rig = sampleRig();
    anim::FaceRigRuntime rt;
    rt.bind(&rig);
    std::vector<anim::FaceTargetOut> out;

    check(rt.expand(idOf("smile"), 0.5f, out) && out.size() == 2, "an explicit pose expands to its targets");
    if (out.size() == 2) {
        check(out[0].target == idOf("mouthSmileLeft"), "target ids are hashes of the names");
        checkNear(out[0].weight, 0.9f * 0.5f, 1e-6f, "scaled by the pose weight");
    }

    // a mesh declaring jawOpen, and a viseme of its own
    rt.setDeclared({idOf("jawOpen"), idOf("viseme_aa"), idOf("custom")}, true);
    out.clear();
    check(rt.expand(idOf("custom"), 0.7f, out) && out.size() == 1 && out[0].target == idOf("custom"), "a mesh target of that name is the identity");
    checkNear(out[0].weight, 0.7f, 1e-6f, "at the pose weight");
    out.clear();
    check(rt.expand(idOf("viseme_aa"), 1.0f, out) && out.size() == 1 && out[0].target == idOf("viseme_aa"),
          "a mesh that carries viseme_aa uses it (no double drive from the built-in composition)");
    out.clear();
    check(rt.expand(idOf("viseme_oh"), 1.0f, out) && out.size() == 3, "a viseme the mesh lacks falls to the built-in composition");

    // explicit beats the mesh target of the same name
    anim::FaceRigAsset own;
    anim::FacePose p;
    p.name = "jawOpen";
    p.targets = {{"mouthClose", 0.5f}};
    own.poses = {p};
    anim::FaceRigRuntime rt2;
    rt2.bind(&own);
    rt2.setDeclared({idOf("jawOpen")}, true);
    out.clear();
    check(rt2.expand(idOf("jawOpen"), 1.0f, out) && out.size() == 1 && out[0].target == idOf("mouthClose"), "an explicit pose beats a mesh target");

    // nothing declared: the built-in rig supplies ARKit identities and visemes; an unknown name does not resolve
    anim::FaceRigRuntime rt3;
    rt3.bind(nullptr);
    out.clear();
    check(rt3.expand(idOf("eyeBlinkLeft"), 1.0f, out) && out.size() == 1 && out[0].target == idOf("eyeBlinkLeft"), "no rig, nothing declared: ARKit names are identities");
    out.clear();
    check(!rt3.expand(idOf("nothingLikeThis"), 1.0f, out) && out.empty(), "an unknown name does not resolve");
}

static fmt::OcSkeleton jawRig() {
    fmt::OcSkeleton s;
    auto bone = [](const char* n, i32 parent) {
        fmt::OcBone b;
        b.name = n;
        b.parent = parent;
        return b;
    };
    s.bones = {bone("root", -1), bone("head", 0), bone("jaw", 1), bone("tip", 2)};
    s.rootBone = 0;
    s.bones[1].rotation = Quat::fromAxisAngle(Vec3{0, 0, 1}, 1.5707963f);   // the head is turned a quarter about Z
    s.bones[2].rotation = Quat::fromAxisAngle(Vec3{0, 1, 0}, 1.5707963f);   // and so is the jaw, about Y
    s.bones[3].translation = Vec3{0, 10, 0};
    return s;
}

static void testJoints() {
    AVER_INFO("=== joint poses on a parented rig with a non-identity rest rotation ===");
    const fmt::OcSkeleton skel = jawRig();
    anim::FaceRigAsset rig;
    anim::FacePose open;
    open.name = "open";
    anim::FaceJoint j;
    j.bone = "jaw";
    j.position = Vec3{0, 2, 0};
    j.rotation = Quat::fromAxisAngle(Vec3{1, 0, 0}, 1.5707963f);
    j.scale = Vec3{1, 2, 1};
    open.joints = {j};
    anim::FaceJoint ghost;
    ghost.bone = "tail";
    open.joints.push_back(ghost);
    rig.poses = {open};
    anim::FaceRigRuntime rt;
    rt.bind(&rig);
    check(rt.hasJoints(idOf("open")), "the pose reports joint deltas");
    check(!rt.hasJoints(idOf("jawOpen")), "a pure-morph pose does not");

    anim::Pose rest;
    anim::restPose(skel, rest);

    anim::Pose full = rest;
    rt.applyJoints(idOf("open"), 1.0f, skel, full);
    // The delta is in the bone's LOCAL frame: a rotation about the jaw's own x axis leaves that axis where it
    // was in the parent's frame (rest.rotate(delta.rotate(X)) == rest.rotate(X)) and turns y. Composed on the
    // wrong side (delta * rest) the x axis would move, because the jaw's rest rotation is not the identity.
    const Vec3 x0 = rest.local[2].rotation.rotate(Vec3{1, 0, 0}), x1 = full.local[2].rotation.rotate(Vec3{1, 0, 0});
    const Vec3 y0 = rest.local[2].rotation.rotate(Vec3{0, 1, 0}), y1 = full.local[2].rotation.rotate(Vec3{0, 1, 0});
    check((x1 - x0).size() < 1e-4f, "the jaw's own x axis is unchanged: the delta acts in the bone's local frame");
    check((y1 - y0).size() > 0.5f, "while its y axis turned");
    check(std::fabs(full.local[2].rotation.x * full.local[2].rotation.x + full.local[2].rotation.y * full.local[2].rotation.y +
                    full.local[2].rotation.z * full.local[2].rotation.z + full.local[2].rotation.w * full.local[2].rotation.w - 1.0f) < 1e-5f,
          "and the rotation stays a unit quaternion");

    checkNear(full.local[2].position.y, 2.0f, 1e-5f, "position adds in the local frame");
    checkNear(full.local[2].scale.y, 2.0f, 1e-5f, "scale multiplies");

    anim::Pose half = rest;
    rt.applyJoints(idOf("open"), 0.5f, skel, half);
    checkNear(half.local[2].position.y, 1.0f, 1e-5f, "weight 0.5: halfway in position");
    checkNear(half.local[2].scale.y, 1.5f, 1e-5f, "and in scale");
    // half the delta: rest * rotX(45 degrees)
    const Quat want = rest.local[2].rotation * Quat::fromAxisAngle(Vec3{1, 0, 0}, 0.7853982f);
    checkNear(half.local[2].rotation.x, want.x, 1e-4f, "and half the angle in rotation (a quarter turn at w = 0.5 is 45 degrees), x");
    checkNear(half.local[2].rotation.y, want.y, 1e-4f, "y");
    checkNear(half.local[2].rotation.z, want.z, 1e-4f, "z");
    checkNear(half.local[2].rotation.w, want.w, 1e-4f, "w");

    anim::Pose none = rest;
    rt.applyJoints(idOf("open"), 0.0f, skel, none);
    check(none.local[2].position.y == rest.local[2].position.y, "weight 0 changes nothing");
    rt.applyJoints(idOf("missing"), 1.0f, skel, none);
    check(none.local[2].position.y == rest.local[2].position.y, "an unknown pose changes nothing");
    check(full.local[1].position.x == rest.local[1].position.x && full.local[3].position.y == rest.local[3].position.y,
          "other bones are untouched; the bone the skeleton lacks was skipped");
    anim::Pose wrong;
    wrong.local.resize(2);
    rt.applyJoints(idOf("open"), 1.0f, skel, wrong);
    check(wrong.local.size() == 2, "a pose of another size is left alone");
}

int main() {
    testAsset();
    testDefaults();
    testResolution();
    testJoints();
    if (g_failures == 0) AVER_INFO("=== all face rig tests passed ===");
    else                 AVER_ERROR("=== {} FAILED ===", g_failures);
    return g_failures == 0 ? 0 : 1;
}
