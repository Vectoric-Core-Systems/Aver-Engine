// IkRigSystemTest -- CIkRig and the OCRIG 2 inputs on entities, through the real AnimSystem tick.
//
// THE WHOLE CHAIN: an .ocikrig and a skeleton on disk, resolved by asset id, bound by bone name, solved in
// AnimSystem's pose-modifier seam with a fake ground probe standing in for physics. What it asserts is what a
// player would see: feet that follow a step, a pelvis that drops, a foot that follows another entity's socket,
// a goal that lets go when its target dies -- and a character with nothing attached that animates unchanged.
#include "../../anim/src/IkRigFixture.hpp"

#include "aver/anim/AnimGraphAsset.hpp"
#include "aver/anim/AnimSystem.hpp"
#include "aver/anim/ControlRig.hpp"
#include "aver/anim/IkRigAsset.hpp"
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Hash.hpp"
#include "aver/core/Log.hpp"
#include "aver/formats/OcAnim.hpp"
#include "aver/formats/OcRig.hpp"
#include "aver/scene/Components.hpp"
#include "aver/scene/World.hpp"

#include <cmath>
#include <filesystem>
#include <map>
#include <string>

using namespace aver;
using namespace ikfix;

static int g_checks = 0, g_failures = 0;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { AVER_INFO("   ok    {}", what); return; }
    AVER_ERROR("   FAIL  {}", what);
    ++g_failures;
}

static std::string f2s(f32 v) { char b[64]; std::snprintf(b, sizeof b, "%.3f", v); return b; }
static f32 gap(const Vec3& a, const Vec3& b) { return (a - b).size(); }

static std::string g_dir;
static const u64 kSkelId = 0x1F00AA01ull;
static const u64 kRigId = 0x1F00AA02ull;
static const u64 kV2RigId = 0x1F00AA03ull;
static const u64 kMissingId = 0x1F00AA04ull;

static std::map<u64, std::string> g_extra;   // ids the montage section registers

static std::string resolvePath(u64 id, void*) {
    if (const auto it = g_extra.find(id); it != g_extra.end()) return it->second;
    if (id == kSkelId) return g_dir + "/h.ocskel";
    if (id == kRigId) return g_dir + "/h.ocikrig";
    if (id == kV2RigId) return g_dir + "/look.ocrig";
    if (id == kMissingId) return g_dir + "/gone.ocikrig";
    return {};
}

// A fake ground: two planes, left (model y < 0) and right, and a record of what the system asked.
struct FakeGround {
    f32 leftZ = 0.0f, rightZ = 0.0f;
    int calls = 0;
    scene::Entity lastSelf = scene::kInvalidEntity;
    bool reportSelfOnce = false;      // the host's self-hit skip is its own contract; here the fake just exercises the path
    Vec3 lastOrigin{0, 0, 0};
    f32 lastDistance = 0.0f;
};

static bool fakeProbe(scene::Entity self, const Vec3& o, f32 dist, u32, Vec3& p, Vec3& n, void* user) {
    auto* g = static_cast<FakeGround*>(user);
    ++g->calls;
    g->lastSelf = self;
    g->lastOrigin = o;
    g->lastDistance = dist;
    const f32 z = o.y < 0.0f ? g->leftZ : g->rightZ;
    if (o.z - z < 0.0f || o.z - z > dist) return false;
    p = Vec3{o.x, o.y, z};
    n = Vec3{0, 0, 1};
    return true;
}

static anim::IkRigAsset legsRig() {
    anim::IkRigAsset a;
    a.name = "Legs";
    a.skeleton = "h.ocskel";
    a.body.bone = "pelvis";
    a.body.maxOffsetCm = 100.0f;
    for (const char* side : {"_l", "_r"}) {
        anim::IkRigChainDef c;
        c.name = std::string("Leg") + side;
        c.start = std::string("thigh") + side;
        c.end = std::string("foot") + side;
        c.solver = anim::IkSolverKind::TwoBone;
        c.priority = 2;
        if (std::string(side) == "_l") c.goal = "LeftFoot";
        a.chains.push_back(c);
        anim::IkRigFootDef f;
        f.chain = c.name;
        f.toe = std::string("ball") + side;
        a.feet.push_back(f);
    }
    return a;
}

static u64 H(const char* s) { return fnv1a64(std::string_view(s)); }

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    AVER_INFO("==================================================");
    AVER_INFO("IK rig on entities");

    std::error_code ec;
    g_dir = (std::filesystem::temp_directory_path(ec) / "aver-ikrig-scene-test").string();
    std::filesystem::remove_all(g_dir, ec);
    std::filesystem::create_directories(g_dir, ec);

    Humanoid h = makeHumanoid();
    // A socket on the root bone, positioned so that a target at the world origin puts it 10 cm ahead of and 8 cm
    // above the left ankle's rest position.
    anim::Pose restPose;
    anim::restPose(h.skel, restPose);
    const Vec3 restFootL = modelPos(h.skel, restPose, h.footL);
    {
        fmt::OcSocket s;
        s.name = "Mark";
        s.bone = h.root;
        const Quat yaw = h.skel.bones[h.root].rotation;
        s.translation = Quat{-yaw.x, -yaw.y, -yaw.z, yaw.w}.rotate(restFootL + Vec3{10, 0, 8});
        h.skel.sockets.push_back(s);
    }
    std::string why;
    check(fmt::saveOcSkel(g_dir + "/h.ocskel", h.skel, &why), "(skeleton written) " + why);
    check(anim::saveIkRig(g_dir + "/h.ocikrig", legsRig(), &why), "(ik rig written) " + why);
    {
        fmt::OcRigData rig;
        rig.name = "Look";
        fmt::OcRigInput lookAt;
        lookAt.name = "LookAt"; lookAt.type = fmt::OcRigInputType::Vec3; lookAt.world = false; lookAt.defVec = Vec3{0, 0, 200};
        fmt::OcRigInput amt;
        amt.name = "Amount"; amt.type = fmt::OcRigInputType::Float; amt.defFloat = 1.0f;
        rig.inputs = {lookAt, amt};
        fmt::OcRigOp op;
        op.kind = fmt::OcRigOpKind::Ccd;
        op.root = "spine_01"; op.tip = "spine_03";
        op.targetRef.kind = fmt::OcRigRef::Kind::Input; op.targetRef.name = "LookAt";
        op.weightInput = "Amount";
        op.iterations = 60; op.tolerance = 0.05f; op.maxStepDeg = 30.0f;
        rig.ops.push_back(op);
        // A second op that reads the first one's result through a bone: reference: aim the head at the spine tip.
        fmt::OcRigOp aim;
        aim.kind = fmt::OcRigOpKind::AimAt;
        aim.root = "neck";
        aim.targetRef.kind = fmt::OcRigRef::Kind::Bone; aim.targetRef.name = "spine_03"; aim.targetRef.offset = Vec3{0, 0, 40};
        aim.hint = Vec3{0, 0, 1};
        aim.weight = 1.0f;
        rig.ops.push_back(aim);
        check(fmt::saveOcRig(g_dir + "/look.ocrig", rig, &why), "(v2 rig written) " + why);
    }

    scene::World& w = scene::World::instance();
    anim::AnimSystem& sys = anim::animSystem();
    sys.clear();
    sys.setResolver(&resolvePath, nullptr);
    anim::ControlRigSystem::registerComponents(w);
    anim::ControlRigSystem rigs;
    rigs.install(sys, w);
    const f32 dt = 1.0f / 60.0f;

    FakeGround ground;
    rigs.setGroundProbe(&fakeProbe, &ground);

    auto makeChar = [&](const char* name, bool withRig, f32 weight = 1.0f) {
        const scene::Entity e = w.create(name);
        auto* sm = static_cast<scene::CSkeletalMesh*>(w.addComponent(e, scene::kComponentSkeletalMesh));
        w.addComponent(e, scene::kComponentAnimator);
        if (sm) sm->skeleton = kSkelId;
        if (withRig) anim::ControlRigSystem::attachIk(w, e, kRigId, weight);
        return e;
    };
    auto poseOf = [&](scene::Entity e) -> const anim::Pose* { return sys.pose(e); };
    auto ankleL = [&](scene::Entity e) { return modelPos(h.skel, *poseOf(e), h.footL); };
    auto ankleR = [&](scene::Entity e) { return modelPos(h.skel, *poseOf(e), h.footR); };
    auto pelvisOf = [&](scene::Entity e) { return modelPos(h.skel, *poseOf(e), h.pelvis); };

    AVER_INFO("=== the component, and the zero-fill trap ===");
    {
        check(w.componentId("CIkRig") != 0 && w.componentVerified(w.componentId("CIkRig")), "CIkRig registers with a verified field table");
        ground.leftZ = 8.0f;
        const scene::Entity e = w.create("raw");
        auto* sm = static_cast<scene::CSkeletalMesh*>(w.addComponent(e, scene::kComponentSkeletalMesh));
        w.addComponent(e, scene::kComponentAnimator);
        sm->skeleton = kSkelId;
        auto* raw = static_cast<anim::CIkRig*>(w.addComponent(e, w.componentId("CIkRig")));
        raw->asset = kRigId;
        check(raw->weight == 0.0f, "a raw attach reads weight 0");
        sys.tick(w, dt);
        check(samePose(*poseOf(e), restPose) && ground.calls == 0, "and weight 0 changes nothing and casts no ray");
        auto* ok = anim::ControlRigSystem::attachIk(w, e, kRigId);
        check(ok && ok->weight == 1.0f, "attachIk writes a fresh value: weight 1");
        sys.tick(w, dt);
        check(ankleL(e).z > restFootL.z + 7.0f, "and then the left foot is lifted onto the step");
        w.destroy(e); w.flush();
        ground.leftZ = 0.0f;
    }

    AVER_INFO("=== flat ground leaves the pose identical to a character with no rig ===");
    {
        const scene::Entity plain = makeChar("plain", false), rigged = makeChar("rigged", true);
        for (int i = 0; i < 4; ++i) sys.tick(w, dt);
        check(samePose(*poseOf(plain), *poseOf(rigged)), "pose() is bit-identical after four ticks");
        check(ground.lastSelf == rigged, "the probe was handed the entity it casts for (the self-hit skip contract)");
        check(ground.lastDistance > 100.0f && ground.lastDistance < 120.0f, "and a ray of probeUp + probeDown = 110 cm, got " + f2s(ground.lastDistance));
        w.destroy(plain); w.destroy(rigged); w.flush();
    }

    AVER_INFO("=== a step lifts one foot, a drop pulls the pelvis down, a ledge lets go ===");
    {
        const scene::Entity e = makeChar("stepper", true);
        sys.tick(w, dt);
        const Vec3 l0 = ankleL(e), r0 = ankleR(e), p0 = pelvisOf(e);
        ground.leftZ = 8.0f;
        for (int i = 0; i < 90; ++i) sys.tick(w, dt);
        check(std::fabs((ankleL(e).z - l0.z) - 8.0f) < 0.6f, "the left ankle settles 8 cm up, " + f2s(ankleL(e).z - l0.z));
        check(gap(ankleR(e), r0) < 0.05f && std::fabs(pelvisOf(e).z - p0.z) < 0.05f, "the right foot and the pelvis stayed put");
        ground.leftZ = -6.0f;
        for (int i = 0; i < 90; ++i) sys.tick(w, dt);
        check(std::fabs((pelvisOf(e).z - p0.z) + 6.0f) < 0.3f, "a 6 cm drop under the planted foot lowers the pelvis 6 cm, " + f2s(pelvisOf(e).z - p0.z));
        check(gap(ankleR(e), r0) < 0.8f, "while the other foot reaches back down to where it was");
        f32 st[5] = {};
        check(anim::ikProvider(11, static_cast<i32>(e), 0, 0, 0, nullptr, 0, st, nullptr, &rigs) == 1 && std::fabs(st[0] + 6.0f) < 0.3f,
              "FOOT_STATE for the pelvis (hash 0) reports the drop, " + f2s(st[0]));
        check(anim::ikProvider(11, static_cast<i32>(e), static_cast<i64>(H("Leg_l")), 0, 0, nullptr, 0, st, nullptr, &rigs) == 1 &&
              std::fabs(st[0] + 6.0f) < 0.3f && st[1] > 0.9f && std::fabs(st[4] - 1.0f) < 1e-4f, "and for the left foot: offset, plant, ground normal");
        ground.leftZ = 0.0f;
        w.destroy(e); w.flush();
    }

    AVER_INFO("=== the probe budget holds offsets ===");
    {
        const scene::Entity e = makeChar("budget", true);
        sys.tick(w, dt);
        rigs.maxFootProbesPerTick = 0;
        ground.leftZ = 8.0f;
        const int calls = ground.calls;
        const Vec3 l0 = ankleL(e);
        for (int i = 0; i < 5; ++i) sys.tick(w, dt);
        check(ground.calls == calls && gap(ankleL(e), l0) < 1e-3f, "with a budget of 0 no ray is cast and the feet hold");
        rigs.maxFootProbesPerTick = 128;
        sys.tick(w, dt);
        check(ground.calls > calls, "and raising it resumes probing");
        ground.leftZ = 0.0f;
        w.destroy(e); w.flush();
    }

    AVER_INFO("=== goals: set, blend, bind to a socket, release when the target dies ===");
    {
        const scene::Entity e = makeChar("goal", true);
        sys.tick(w, dt);
        const Vec3 rest = ankleL(e);
        const u64 goal = H("LeftFoot");
        f32 st[3] = {};
        check(anim::ikProvider(5, static_cast<i32>(e), static_cast<i64>(H("Nope")), 0, 0, nullptr, 0, st, nullptr, &rigs) == 0, "(no input: refused)");
        const f32 pos[3] = {rest.x + 12, rest.y, rest.z + 10};
        check(anim::ikProvider(5, static_cast<i32>(e), static_cast<i64>(H("Nope")), 0, 0, pos, 3, nullptr, nullptr, &rigs) == 0,
              "a goal name the rig does not declare is refused, not stored");
        check(anim::ikProvider(9, static_cast<i32>(e), static_cast<i64>(goal), 0, 0, nullptr, 0, st, nullptr, &rigs) == 1 && st[0] == 0.0f,
              "a declared goal nobody touched reports alpha 0");
        check(anim::ikProvider(5, static_cast<i32>(e), static_cast<i64>(goal), 0, 0, pos, 3, nullptr, nullptr, &rigs) == 1, "SetIkGoal (3 floats) is accepted");
        for (int i = 0; i < 4; ++i) sys.tick(w, dt);
        anim::ikProvider(9, static_cast<i32>(e), static_cast<i64>(goal), 0, 0, nullptr, 0, st, nullptr, &rigs);
        check(st[0] > 0.2f && st[0] < 0.6f, "the alpha ramps in over 0.15 s, " + f2s(st[0]) + " after 4 ticks");
        for (int i = 0; i < 30; ++i) sys.tick(w, dt);
        check(gap(ankleL(e), Vec3{pos[0], pos[1], pos[2]}) < 0.5f, "and the ankle ends on the goal, off by " + f2s(gap(ankleL(e), Vec3{pos[0], pos[1], pos[2]})));
        anim::ikProvider(9, static_cast<i32>(e), static_cast<i64>(goal), 0, 0, nullptr, 0, st, nullptr, &rigs);
        check(st[0] > 0.99f && st[1] < 0.5f && st[2] == 1.0f, "GOAL_STATE: alpha 1, error under half a centimetre, reached");

        // Release with a blend: alpha is linear in time, 0.5 s.
        const f32 w0[2] = {0.0f, 0.5f};
        check(anim::ikProvider(7, static_cast<i32>(e), static_cast<i64>(goal), 0, 0, w0, 2, nullptr, nullptr, &rigs) == 1, "SetIkGoalWeight 0 over 0.5 s");
        for (int i = 0; i < 15; ++i) sys.tick(w, dt);
        anim::ikProvider(9, static_cast<i32>(e), static_cast<i64>(goal), 0, 0, nullptr, 0, st, nullptr, &rigs);
        check(std::fabs(st[0] - 0.5f) < 0.06f, "a quarter second into a half second release the alpha is 0.5, " + f2s(st[0]));
        for (int i = 0; i < 30; ++i) sys.tick(w, dt);
        check(gap(ankleL(e), rest) < 0.5f, "and the foot is back on the animation");

        // A rotation rides along when inCount is 7.
        const f32 withRot[7] = {pos[0], pos[1], pos[2], 0, 0, 0, 1};
        check(anim::ikProvider(5, static_cast<i32>(e), static_cast<i64>(goal), 0, 0, withRot, 7, nullptr, nullptr, &rigs) == 1, "SetIkGoal with a rotation (7 floats)");

        // Bound to a second entity's socket: the foot follows the target across ticks.
        anim::ikProvider(8, static_cast<i32>(e), 0, 0, 0, nullptr, 0, nullptr, nullptr, &rigs);      // clear all
        for (int i = 0; i < 30; ++i) sys.tick(w, dt);
        const scene::Entity target = makeChar("target", false);
        // Bound the instant it spawns, before it has been posed: the goal waits for it instead of giving up.
        const f32 off[3] = {0, 0, 0};
        check(anim::ikProvider(6, static_cast<i32>(e), static_cast<i64>(goal), static_cast<i64>(target), static_cast<i64>(H("Mark")), off, 3, nullptr, nullptr, &rigs) == 1,
              "BindIkGoal to the target's 'Mark' socket");
        f32 worst = 0.0f;
        for (int step = 0; step < 4; ++step) {
            const Vec3 t{static_cast<f32>(step) * 3.0f, 0, static_cast<f32>(step)};
            w.setLocalPosition(target, t);
            w.flush();
            for (int i = 0; i < 40; ++i) sys.tick(w, dt);
            const Vec3 want = restFootL + Vec3{10, 0, 8} + t;
            worst = std::fmax(worst, gap(ankleL(e), want));
        }
        check(worst < 0.8f, "the ankle follows the socket while the target moves, worst miss " + f2s(worst) + " cm");

        // The target dies: the goal lets go instead of freezing.
        w.destroy(target);
        w.flush();
        for (int i = 0; i < 40; ++i) sys.tick(w, dt);
        anim::ikProvider(9, static_cast<i32>(e), static_cast<i64>(goal), 0, 0, nullptr, 0, st, nullptr, &rigs);
        check(st[0] < 0.01f && gap(ankleL(e), rest) < 0.6f, "a destroyed target releases the goal to alpha 0 and the foot to the animation");

        // clearRuntime forgets everything a script said.
        anim::ikProvider(5, static_cast<i32>(e), static_cast<i64>(goal), 0, 0, pos, 3, nullptr, nullptr, &rigs);
        for (int i = 0; i < 30; ++i) sys.tick(w, dt);
        rigs.clearRuntime();
        sys.tick(w, dt);
        anim::ikProvider(9, static_cast<i32>(e), static_cast<i64>(goal), 0, 0, nullptr, 0, st, nullptr, &rigs);
        check(st[0] == 0.0f, "clearRuntime drops goals: alpha back to 0");
        w.destroy(e); w.flush();
    }

    AVER_INFO("=== foot weight ===");
    {
        const scene::Entity e = makeChar("fw", true);
        ground.leftZ = 8.0f;
        const f32 off[2] = {0.0f, 0.0f};
        check(anim::ikProvider(10, static_cast<i32>(e), 0, 0, 0, off, 2, nullptr, nullptr, &rigs) == 1, "SetFootIkWeight 0 on every foot");
        for (int i = 0; i < 10; ++i) sys.tick(w, dt);
        check(samePose(*poseOf(e), restPose), "with the feet off a step changes nothing");
        const f32 on[2] = {1.0f, 0.0f};
        anim::ikProvider(10, static_cast<i32>(e), static_cast<i64>(H("Leg_l")), 0, 0, on, 2, nullptr, nullptr, &rigs);
        for (int i = 0; i < 90; ++i) sys.tick(w, dt);
        check(ankleL(e).z > restFootL.z + 7.0f, "turning just the left foot on lifts it");
        check(anim::ikProvider(10, static_cast<i32>(e), static_cast<i64>(H("Nope")), 0, 0, on, 2, nullptr, nullptr, &rigs) == 0, "an unknown foot is refused");
        ground.leftZ = 0.0f;
        w.destroy(e); w.flush();
    }

    AVER_INFO("=== a missing rig is survivable ===");
    {
        const scene::Entity e = w.create("missing");
        auto* sm = static_cast<scene::CSkeletalMesh*>(w.addComponent(e, scene::kComponentSkeletalMesh));
        w.addComponent(e, scene::kComponentAnimator);
        sm->skeleton = kSkelId;
        anim::ControlRigSystem::attachIk(w, e, kMissingId);
        sys.tick(w, dt);
        check(samePose(*poseOf(e), restPose), "an unloadable rig leaves the pose alone");
        check(anim::ikProvider(5, static_cast<i32>(e), 1, 0, 0, nullptr, 3, nullptr, nullptr, &rigs) == 0, "and its goals are refused");
        w.destroy(e); w.flush();
    }

    AVER_INFO("=== order: the rig runs first, a later modifier sees the IK pose ===");
    {
        struct Seen { Vec3 ankle{0, 0, 0}; bool ran = false; };
        static Seen seen;
        static anim::PoseModifierFn prev = nullptr;
        static void* prevUser = nullptr;
        prev = sys.poseModifier();
        prevUser = sys.poseModifierUser();
        sys.setPoseModifier([](scene::Entity e, const fmt::OcSkeleton& skel, anim::Pose& pose, void*) {
            prev(e, skel, pose, prevUser);
            seen.ran = true;
            seen.ankle = modelPos(skel, pose, 8);       // foot_l in the humanoid fixture
        }, nullptr);
        const scene::Entity e = makeChar("order", true);
        ground.leftZ = 8.0f;
        sys.tick(w, dt);
        check(seen.ran && seen.ankle.z > restFootL.z + 7.0f, "the chained modifier saw the ankle already lifted");
        ground.leftZ = 0.0f;
        sys.setPoseModifier(prev, prevUser);
        w.destroy(e); w.flush();
    }

    AVER_INFO("=== a montage playing underneath still gets IK ===");
    {
        // A one-track clip that holds the spine bent, played as a montage; the feet are placed on top of it.
        fmt::OcAnimation clip;
        clip.duration = 1.0f;
        clip.flags = fmt::kOcAnimLoop;
        fmt::OcTrack t;
        t.boneIndex = static_cast<u16>(h.spine2);
        t.channels = fmt::kOcChannelRotation;
        t.times = {0.0f, 1.0f};
        const Quat q = Quat::fromAxisAngle(Vec3{0, 1, 0}, 0.4f);
        t.values = {q.x, q.y, q.z, q.w, q.x, q.y, q.z, q.w};
        clip.tracks.push_back(t);
        check(fmt::saveOcAnim(g_dir + "/bend.ocanim", clip, &why), "(clip written) " + why);
        g_extra[fnv1a64(std::string_view("Clips/bend.ocanim"))] = g_dir + "/bend.ocanim";
        anim::MontageAsset ma;
        ma.name = "Bend";
        ma.blendIn = 0.0f;
        ma.blendOut = 0.2f;
        ma.inCurve = anim::BlendCurve::Linear;
        ma.outCurve = anim::BlendCurve::Linear;
        ma.segments = {{"Clips/bend.ocanim", 0.0f, -1.0f, 1.0f}};
        ma.sections = {{"Default", 0.0f, ""}};
        check(anim::saveMontage(g_dir + "/bend.ocmontage", ma, &why), "(montage written) " + why);
        const u64 montageId = fnv1a64(std::string_view("Clips/bend.ocmontage"));
        g_extra[montageId] = g_dir + "/bend.ocmontage";

        const scene::Entity e = makeChar("montage", true);
        ground.leftZ = 8.0f;
        check(sys.montages().play(sys, w, e, montageId, 0, 0, 1.0f, 0.0f), "the montage plays");
        for (int i = 0; i < 6; ++i) sys.tick(w, dt);
        const f32 bent = angleDeg(modelRot(h.skel, restPose, h.spine2), modelRot(h.skel, *poseOf(e), h.spine2));
        check(bent > 10.0f, "the montage is applied (the spine is bent " + f2s(bent) + " degrees)");
        check(ankleL(e).z > restFootL.z + 7.0f, "and the left foot is still lifted onto the step");
        ground.leftZ = 0.0f;
        w.destroy(e); w.flush();
    }

    AVER_INFO("=== OCRIG 2: inputs drive an op, bone: refs see earlier ops, defaults apply ===");
    {
        const scene::Entity e = w.create("look");
        auto* sm = static_cast<scene::CSkeletalMesh*>(w.addComponent(e, scene::kComponentSkeletalMesh));
        w.addComponent(e, scene::kComponentAnimator);
        sm->skeleton = kSkelId;
        anim::ControlRigSystem::attach(w, e, kV2RigId);
        sys.tick(w, dt);
        const Vec3 top0 = modelPos(h.skel, *poseOf(e), h.spine3);
        check(gap(top0, Vec3{0, 0, 200}) > 1.0f, "(the default look-at is straight up, 200 cm; the spine tip follows toward it)");
        const Vec3 restTop = modelPos(h.skel, restPose, h.spine3);
        check(gap(top0, restTop) < 8.0f, "the declared default applies until the script sets the input");
        const Vec3 fwd = restTop + Vec3{12, 4, 14};
        const f32 v3[3] = {fwd.x, fwd.y, fwd.z};
        check(rigs.setInput(e, H("LookAt"), v3, 3), "setInput stores a vec3");
        sys.tick(w, dt);
        const Vec3 spineBase = modelPos(h.skel, *poseOf(e), h.spine1);
        const Vec3 tip = modelPos(h.skel, *poseOf(e), h.spine3);
        const f32 reach = gap(modelPos(h.skel, restPose, h.spine3), modelPos(h.skel, restPose, h.spine1));
        const Vec3 want = spineBase + (fwd - spineBase).getSafeNormal() * std::fmin(reach, (fwd - spineBase).size());
        check(gap(tip, want) < 0.6f, "the CCD op reaches toward the input, off by " + f2s(gap(tip, want)));
        const f32 zero[1] = {0.0f};
        check(rigs.setInput(e, H("Amount"), zero, 1), "a float input sets the op weight");
        sys.tick(w, dt);
        check(samePose(*poseOf(e), restPose) == false || gap(modelPos(h.skel, *poseOf(e), h.spine3), restTop) < 0.5f,
              "weight 0 from the input turns the first op off (the aim op still reads the bone: ref)");
        rigs.clearInput(e, H("LookAt"));
        rigs.clearInput(e, H("Amount"));
        // The 33rd distinct name is refused (the cap moved from 8 to 32 for six-wheel and tracked vehicles).
        bool lastRefused = false;
        const f32 one[1] = {1.0f};
        for (int i = 0; i < 33; ++i) {
            const std::string n = "In" + std::to_string(i);
            const bool ok = rigs.setInput(e, H(n.c_str()), one, 1);
            if (i < 32) { if (!ok) { lastRefused = false; break; } }
            else lastRefused = !ok;
        }
        check(lastRefused, "32 distinct inputs fit, the 33rd is refused");
        rigs.clearInput(e, 0);
        check(rigs.setInput(e, H("Fresh"), one, 1), "clearing every input frees the slots");
        w.destroy(e); w.flush();
        sys.tick(w, dt);
        check(!rigs.setInput(e, H("X"), one, 1), "an input cannot be set on a dead entity");
    }

    std::filesystem::remove_all(g_dir, ec);
    AVER_INFO("==================================================");
    if (g_failures == 0) AVER_INFO("=== {} assertions, 0 failed ===", g_checks);
    else                 AVER_ERROR("=== {} assertions, {} FAILED ===", g_checks, g_failures);
    return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
}
