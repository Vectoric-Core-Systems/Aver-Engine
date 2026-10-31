// PoseSeamTest -- the script pose seam on an entity: bone overrides written, applied inside the control rig's
// modifier AFTER the rig's ops and the clip, and read back; the chain order against the rig and the ragdoll's
// slot (the modifier a later system chains); the provider the framework relays to. No GPU, no scripting host.
#include "aver/anim/AnimSystem.hpp"
#include "aver/anim/ControlRig.hpp"
#include "aver/anim/PoseSeam.hpp"
#include "aver/core/Hash.hpp"
#include "aver/core/Log.hpp"
#include "aver/formats/OcAnim.hpp"
#include "aver/formats/OcRig.hpp"
#include "aver/scene/Components.hpp"
#include "aver/scene/World.hpp"

#include <cmath>
#include <cstring>
#include <filesystem>
#include <string>

using namespace aver;

static int g_checks = 0, g_failures = 0;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { AVER_INFO("   ok    {}", what); return; }
    AVER_ERROR("   FAIL  {}", what);
    ++g_failures;
}

static std::string f2s(f32 v) { char b[64]; std::snprintf(b, sizeof b, "%.3f", v); return b; }
static bool near3(const Vec3& a, const Vec3& b, f32 eps = 1e-3f) {
    return std::fabs(a.x - b.x) < eps && std::fabs(a.y - b.y) < eps && std::fabs(a.z - b.z) < eps;
}

static std::string g_dir;
static const u64 kSkelId = 0x5EA00001ull;
static const u64 kRigId  = 0x5EA00002ull;

static std::string resolvePath(u64 id, void*) {
    if (id == kSkelId) return g_dir + "/arm.ocskel";
    if (id == kRigId)  return g_dir + "/pull.ocrig";
    return {};
}

// shoulder -> elbow (40 up) -> wrist (60 further up).
static fmt::OcSkeleton armSkeleton() {
    fmt::OcSkeleton s;
    fmt::OcBone shoulder; shoulder.name = "shoulder"; shoulder.parent = -1;
    fmt::OcBone elbow;    elbow.name = "elbow";       elbow.parent = 0; elbow.translation = Vec3{0, 0, 40};
    fmt::OcBone wrist;    wrist.name = "wrist";       wrist.parent = 1; wrist.translation = Vec3{0, 0, 60};
    s.bones = {shoulder, elbow, wrist};
    s.rootBone = 0;
    return s;
}

static Vec3 modelPos(anim::PoseSeamSystem& seam, scene::Entity e, u32 bone, bool before = false) {
    Transform t;
    seam.readBone(e, bone, anim::PoseReadSpace::Model, before, t);
    return t.position;
}

// What a chained modifier (the ragdoll's place) sees: records the elbow's local position when it runs.
static Vec3 g_seenByChained{0, 0, 0};
static int g_chainedCalls = 0;
static anim::PoseModifierFn g_prev = nullptr;
static void* g_prevUser = nullptr;
static void chained(scene::Entity e, const fmt::OcSkeleton& skel, anim::Pose& pose, void* user) {
    (void)user;
    if (g_prev) g_prev(e, skel, pose, g_prevUser);
    if (pose.local.size() > 1) g_seenByChained = pose.local[1].position;
    ++g_chainedCalls;
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    AVER_INFO("==================================================");
    AVER_INFO("pose seam: script bone overrides and reads");

    std::error_code ec;
    g_dir = (std::filesystem::temp_directory_path(ec) / "aver-poseseam-test").string();
    std::filesystem::remove_all(g_dir, ec);
    std::filesystem::create_directories(g_dir, ec);

    const fmt::OcSkeleton skel = armSkeleton();
    {
        std::string why;
        check(fmt::saveOcSkel(g_dir + "/arm.ocskel", skel, &why), "the skeleton writes: " + why);
        // A rig that pulls the wrist to a goal, to prove the seam runs AFTER the rig's ops.
        fmt::OcRigData rig;
        rig.name = "Pull";
        fmt::OcRigOp op;
        op.kind = fmt::OcRigOpKind::TwoBoneIk;
        op.root = "shoulder"; op.mid = "elbow"; op.tip = "wrist";
        op.target = Vec3{80, 0, 0};
        op.hint = Vec3{0, -100, 20};
        op.weight = 1.0f;
        rig.ops.push_back(op);
        check(fmt::saveOcRig(g_dir + "/pull.ocrig", rig, &why), "the rig writes: " + why);
    }

    scene::World& w = scene::World::instance();
    anim::AnimSystem& sys = anim::animSystem();
    sys.clear();
    sys.setResolver(&resolvePath, nullptr);

    AVER_INFO("=== registration ===");
    anim::ControlRigSystem rigs;
    const u32 rigType = rigs.registerComponents(w);
    check(rigType != 0, "the control rig registered");
    const u32 type = anim::PoseSeamSystem::typeId();
    check(type != 0 && w.componentId("CPoseSeam") == type, "CPoseSeam is registered by the rig system, under its name");
    check(w.componentVerified(type), "its field table covers the struct with no gap");
    rigs.install(sys, w);
    anim::PoseSeamSystem& seam = rigs.poseSeam();

    const scene::Entity e = w.create("arm");
    {
        auto* sm = static_cast<scene::CSkeletalMesh*>(w.addComponent(e, scene::kComponentSkeletalMesh));
        w.addComponent(e, scene::kComponentAnimator);
        if (sm) sm->skeleton = kSkelId;
    }

    AVER_INFO("=== reads before any write ===");
    sys.tick(w, 0.016f);
    check(seam.boneCount(e) == 3, "three bones are posed");
    check(seam.findBone(e, "elbow") == 1 && seam.findBone(e, "nope") == -1, "bones are found by name");
    {
        Transform rest;
        i32 parent = -2;
        check(seam.restBone(e, 2, rest, parent) && parent == 1 && near3(rest.position, Vec3{0, 0, 60}), "the rest transform and parent read back");
        check(near3(modelPos(seam, e, 2), Vec3{0, 0, 100}), "the wrist sits 100 up in model space at rest: " + f2s(modelPos(seam, e, 2).z));
        check(seam.overrideCount(e) == 0, "no bone carries an override");
    }

    AVER_INFO("=== a local override moves the bone and its children ===");
    {
        Transform t;
        t.position = Vec3{0, 0, 55};
        check(seam.setBone(e, 1, t, anim::kPoseSeamPosition, 1.0f), "the elbow position is written");
        check(w.component<anim::CPoseSeam>(e, type) != nullptr, "the first write attached CPoseSeam");
        check(seam.overrideCount(e) == 1, "one override is live");
        sys.tick(w, 0.016f);
        check(near3(modelPos(seam, e, 1), Vec3{0, 0, 55}), "the elbow is at its override");
        check(near3(modelPos(seam, e, 2), Vec3{0, 0, 115}), "and the wrist followed: " + f2s(modelPos(seam, e, 2).z));
        check(near3(modelPos(seam, e, 1, true), Vec3{0, 0, 40}), "a read before the seam still sees the sampled elbow");
        sys.tick(w, 0.016f);
        check(near3(modelPos(seam, e, 1), Vec3{0, 0, 55}), "the override persists across ticks");
        check(seam.clearBone(e, 1), "it clears");
        sys.tick(w, 0.016f);
        check(near3(modelPos(seam, e, 1), Vec3{0, 0, 40}), "and the elbow returns to the pose underneath");
    }

    AVER_INFO("=== channels, amount and the master weight ===");
    {
        Transform t;
        t.position = Vec3{0, 0, 80};
        t.rotation = Quat::fromAxisAngle(Vec3{0, 1, 0}, 1.0f);
        check(seam.setBone(e, 1, t, anim::kPoseSeamPosition, 0.5f), "a half-amount position override");
        sys.tick(w, 0.016f);
        check(near3(modelPos(seam, e, 1), Vec3{0, 0, 60}), "the position is halfway between 40 and 80: " + f2s(modelPos(seam, e, 1).z));
        Transform got;
        seam.readBone(e, 1, anim::PoseReadSpace::Local, false, got);
        check(std::fabs(got.rotation.w - 1.0f) < 1e-5f, "a position-only override leaves the rotation alone");
        check(seam.setWeight(e, 0.5f), "the master weight is set");
        sys.tick(w, 0.016f);
        check(near3(modelPos(seam, e, 1), Vec3{0, 0, 50}), "amount 0.5 times weight 0.5 is a quarter of the way: " + f2s(modelPos(seam, e, 1).z));
        check(seam.setEnabled(e, false), "the seam switches off");
        sys.tick(w, 0.016f);
        check(near3(modelPos(seam, e, 1), Vec3{0, 0, 40}), "off leaves the pose underneath");
        check(seam.setEnabled(e, true) && seam.setWeight(e, 1.0f), "and back on");
        seam.clearBone(e, -1);
        check(seam.overrideCount(e) == 0, "clear -1 drops every override");
    }

    AVER_INFO("=== a model-space override pins a bone and the parent chain is unwound ===");
    {
        Transform shoulder;
        shoulder.position = Vec3{10, 0, 0};
        seam.setBone(e, 0, shoulder, anim::kPoseSeamPosition, 1.0f);
        Transform want;
        want.position = Vec3{30, 20, 70};
        check(seam.setBone(e, 1, want, anim::kPoseSeamPosition | anim::kPoseSeamModel, 1.0f), "the elbow is pinned in model space");
        sys.tick(w, 0.016f);
        check(near3(modelPos(seam, e, 0), Vec3{10, 0, 0}), "the shoulder moved to its local override");
        check(near3(modelPos(seam, e, 1), Vec3{30, 20, 70}), "the elbow lands on the model-space target: " + f2s(modelPos(seam, e, 1).x));
        seam.clearBone(e, -1);
    }

    AVER_INFO("=== once-overrides drop themselves ===");
    {
        Transform t;
        t.position = Vec3{0, 0, 90};
        seam.setBone(e, 1, t, anim::kPoseSeamPosition | anim::kPoseSeamOnce, 1.0f);
        sys.tick(w, 0.016f);
        check(near3(modelPos(seam, e, 1), Vec3{0, 0, 90}), "applied on the tick");
        check(seam.overrideCount(e) == 0, "and gone from the count");
        sys.tick(w, 0.016f);
        check(near3(modelPos(seam, e, 1), Vec3{0, 0, 40}), "the next tick has the pose underneath again");
    }

    AVER_INFO("=== refusals ===");
    {
        Transform t;
        check(!seam.setBone(e, 9, t, anim::kPoseSeamAll), "a bone out of range is refused");
        check(!seam.setBone(e, 0, t, 0), "no channels is refused");
        t.position = Vec3{NAN, 0, 0};
        check(!seam.setBone(e, 0, t, anim::kPoseSeamAll), "a non-finite value is refused");
        const scene::Entity bare = w.create("bare");
        Transform ok;
        check(!seam.setBone(bare, 0, ok, anim::kPoseSeamAll), "an entity with no skeleton is refused");
        check(!seam.setWeight(bare, 0.5f), "and has no weight to set");
    }

    AVER_INFO("=== the order: clip -> rig -> seam -> a chained modifier (the ragdoll's slot) ===");
    {
        const scene::Entity r = w.create("rigged");
        auto* sm = static_cast<scene::CSkeletalMesh*>(w.addComponent(r, scene::kComponentSkeletalMesh));
        w.addComponent(r, scene::kComponentAnimator);
        if (sm) sm->skeleton = kSkelId;
        anim::ControlRigSystem::attach(w, r, kRigId);
        sys.tick(w, 0.016f);
        const Vec3 afterRig = modelPos(seam, r, 2);
        check(near3(afterRig, Vec3{80, 0, 0}, 1.0f), "the rig pulls the wrist onto its goal: x " + f2s(afterRig.x));

        Transform t;
        t.rotation = Quat::fromAxisAngle(Vec3{0, 1, 0}, 0.7f);
        check(seam.setBone(r, 2, t, anim::kPoseSeamRotation, 1.0f), "a wrist rotation is written over the rigged pose");
        sys.tick(w, 0.016f);
        check(near3(modelPos(seam, r, 2), afterRig, 1e-2f), "the wrist POSITION is still where the rig left it (the seam ran after the rig)");
        Transform before, after;
        seam.readBone(r, 2, anim::PoseReadSpace::Local, true, before);
        seam.readBone(r, 2, anim::PoseReadSpace::Local, false, after);
        check(std::fabs(before.rotation.w - after.rotation.w) > 1e-3f, "the final rotation differs from the one before the seam");

        // A modifier installed after the rig system (the ragdoll does this) sees the seam's result.
        g_prev = sys.poseModifier();
        g_prevUser = sys.poseModifierUser();
        sys.setPoseModifier(&chained, nullptr);
        seam.setBone(r, 1, Transform{Vec3{0, 0, 33}, Quat::identity(), Vec3{1, 1, 1}}, anim::kPoseSeamPosition, 1.0f);
        g_chainedCalls = 0;
        sys.tick(w, 0.016f);
        check(g_chainedCalls > 0, "the chained modifier ran");
        check(std::fabs(g_seenByChained.z - 33.0f) < 1e-3f, "and saw the seam's elbow, not the rig's: " + f2s(g_seenByChained.z));
        sys.setPoseModifier(g_prev, g_prevUser);
    }

    AVER_INFO("=== the provider the framework relays to ===");
    {
        seam.clearBone(e, -1);
        float out[10] = {};
        char name[16] = {};
        i64 outI[1] = {-1};
        check(anim::poseProvider(1, static_cast<i32>(e), 0, 0, 0, nullptr, 0, nullptr, nullptr, nullptr, 0, &seam) == 3, "BONE_COUNT returns 3");
        check(anim::poseProvider(2, static_cast<i32>(e), static_cast<i64>(fnv1a64(std::string_view("wrist"))), 0, 0, nullptr, 0, nullptr, outI, nullptr, 0, &seam) == 1 && outI[0] == 2,
              "FIND resolves a name hash");
        check(anim::poseProvider(3, static_cast<i32>(e), 1, 0, 0, nullptr, 0, out, outI, name, sizeof name, &seam) == 1 && std::strcmp(name, "elbow") == 0 && outI[0] == 0 &&
                  std::fabs(out[2] - 40.0f) < 1e-3f && std::fabs(out[6] - 1.0f) < 1e-5f && std::fabs(out[7] - 1.0f) < 1e-5f,
              "INFO gives the name, the parent and the rest transform");
        char tiny[3] = {};
        anim::poseProvider(3, static_cast<i32>(e), 1, 0, 0, nullptr, 0, out, outI, tiny, sizeof tiny, &seam);
        check(std::strcmp(tiny, "el") == 0, "a short name buffer is cut and still terminated");
        const float in[11] = {0, 0, 70, 0, 0, 0, 1, 1, 1, 1, 1.0f};
        check(anim::poseProvider(5, static_cast<i32>(e), 1, anim::kPoseSeamPosition, 0, in, 11, nullptr, nullptr, nullptr, 0, &seam) == 1, "WRITE is accepted");
        sys.tick(w, 0.016f);
        check(anim::poseProvider(4, static_cast<i32>(e), 1, 1, 0, nullptr, 0, out, nullptr, nullptr, 0, &seam) == 1 && std::fabs(out[2] - 70.0f) < 1e-3f, "READ model sees it");
        check(anim::poseProvider(9, static_cast<i32>(e), 0, 0, 0, nullptr, 0, nullptr, nullptr, nullptr, 0, &seam) == 1, "OVERRIDES counts one");
        float range[20] = {};
        for (int k = 0; k < 2; ++k) { range[k * 10 + 3 + 3] = 1.0f; range[k * 10 + 7] = range[k * 10 + 8] = range[k * 10 + 9] = 1.0f; range[k * 10 + 2] = 10.0f + 5.0f * k; }
        check(anim::poseProvider(10, static_cast<i32>(e), anim::kPoseSeamAll, 0, 0, range, 20, nullptr, nullptr, nullptr, 0, &seam) == 1, "WRITE_RANGE writes two bones");
        check(seam.overrideCount(e) == 2, "and two overrides are live");
        check(anim::poseProvider(6, static_cast<i32>(e), -1, 0, 0, nullptr, 0, nullptr, nullptr, nullptr, 0, &seam) == 1 && seam.overrideCount(e) == 0, "CLEAR -1 drops them");
        check(anim::poseProvider(99, static_cast<i32>(e), 0, 0, 0, nullptr, 0, nullptr, nullptr, nullptr, 0, &seam) == 0, "an unknown op is refused");
        check(anim::poseProvider(1, static_cast<i32>(e), 0, 0, 0, nullptr, 0, nullptr, nullptr, nullptr, 0, nullptr) == 0, "no user is refused");
    }

    AVER_INFO("=== a destroyed entity's state is pruned ===");
    {
        const scene::Entity d = w.create("doomed");
        auto* sm = static_cast<scene::CSkeletalMesh*>(w.addComponent(d, scene::kComponentSkeletalMesh));
        w.addComponent(d, scene::kComponentAnimator);
        if (sm) sm->skeleton = kSkelId;
        sys.tick(w, 0.016f);
        Transform t;
        t.position = Vec3{0, 0, 1};
        seam.setBone(d, 0, t, anim::kPoseSeamPosition);
        check(seam.overrideCount(d) == 1, "the doomed entity has an override");
        w.destroy(d);
        sys.tick(w, 0.016f);
        sys.tick(w, 0.016f);
        check(seam.overrideCount(d) == 0, "which is gone after it dies");
    }

    rigs.uninstall(sys);
    std::filesystem::remove_all(g_dir, ec);
    AVER_INFO("{} checks, {} failures", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
