// RagdollSystemTest -- a skinned entity's skeleton as a real physics ragdoll, through AnimSystem's
// pose-modifier seam, in a real physics world (aver_phys_init) with no GPU.
//
// WHAT IT PINS: that installing CHAINS the modifier already there; that the world <-> local pose
// conversion round-trips for an entity that is moved, rotated and scaled (the quiet way this feature
// goes wrong); that a limp ragdoll falls and comes to rest on a floor; that the blend weight ramps;
// that stopping gives the animation back and destroys the physics ragdoll; and that an impulse moves
// the part it names.
#include "aver/anim/AnimSystem.hpp"
#include "aver/anim/Pose.hpp"
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Hash.hpp"
#include "aver/core/Log.hpp"
#include "aver/formats/OcAnim.hpp"
#include "aver/physics/physics_abi.h"
#include "aver/physics/physics_ragdoll_abi.h"
#include "aver/ragdoll/RagdollSystem.hpp"
#include "aver/scene/Components.hpp"
#include "aver/scene/World.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>

using namespace aver;

static int g_checks = 0, g_failures = 0;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { AVER_INFO("   ok    {}", what); return; }
    AVER_ERROR("   FAIL  {}", what);
    ++g_failures;
}

static std::string f2s(f32 v) { char b[64]; std::snprintf(b, sizeof b, "%.2f", v); return b; }

static std::string g_dir;
static const u64 kSkelId = 0x4A6D0111ull;

static const u64 kAuthoredId = 0x4A6D0112ull;   // the same body with an .ocragdoll override file beside it

static std::string resolvePath(u64 id, void*) {
    if (id == kSkelId) return g_dir + "/body.ocskel";
    if (id == kAuthoredId) return g_dir + "/authored.ocskel";
    return std::string{};
}

enum Bone : int { Root, Pelvis, Spine, Chest, Neck, Head, ThighL, KneeL, FootL, UpperArmL, ForearmL, HandL, BoneCount };

// ~170 cm, Z up, a transform node over the pelvis like most imported rigs have.
static fmt::OcSkeleton body() {
    struct Def { const char* name; int parent; Vec3 t; };
    const Def defs[BoneCount] = {
        {"Root", -1, {0, 0, 0}},       {"Pelvis", Root, {0, 0, 95}}, {"Spine", Pelvis, {0, 0, 12}},
        {"Chest", Spine, {0, 0, 18}},  {"Neck", Chest, {0, 0, 20}},  {"Head", Neck, {0, 0, 10}},
        {"ThighL", Pelvis, {0, -9, -4}}, {"KneeL", ThighL, {0, 0, -42}}, {"FootL", KneeL, {0, 0, -40}},
        {"UpperArmL", Chest, {0, -16, -2}}, {"ForearmL", UpperArmL, {0, -28, 0}}, {"HandL", ForearmL, {0, -25, 0}},
    };
    fmt::OcSkeleton s;
    for (const Def& d : defs) {
        fmt::OcBone b;
        b.name = d.name;
        b.parent = d.parent;
        b.translation = d.t;
        s.bones.push_back(b);
    }
    s.rootBone = 0;
    return s;
}

static scene::Entity makeBody(scene::World& w, const char* name, const Transform& xf) {
    const scene::Entity e = w.create(name);
    auto* sm = static_cast<scene::CSkeletalMesh*>(w.addComponent(e, scene::kComponentSkeletalMesh));
    w.addComponent(e, scene::kComponentAnimator);      // no clip: the rest pose is the animation
    if (sm) sm->skeleton = kSkelId;
    w.setLocalTransform(e, xf);
    w.flush();
    return e;
}

static Vec3 boneModel(const fmt::OcSkeleton& skel, const anim::Pose& p, int bone) {
    std::vector<Mat4> model;
    anim::poseToModel(skel, p, model);
    return {model[static_cast<usize>(bone)].m[3][0], model[static_cast<usize>(bone)].m[3][1],
            model[static_cast<usize>(bone)].m[3][2]};
}

// One frame in the order the hosts run it: physics, ragdoll, animation, world flush.
static void frame(scene::World& w, f32 dt) {
    aver_phys_step(dt);
    ragdoll::ragdollSystem().tick(dt);
    anim::animSystem().tick(w, dt);
    w.flush();
}

static void frames(scene::World& w, f32 seconds) {
    const f32 dt = 1.0f / 60.0f;
    for (f32 t = 0; t < seconds; t += dt) frame(w, dt);
}

// A modifier standing in for the control rig, to prove the ragdoll calls the one it replaced.
static int g_chained = 0;
static void countingModifier(scene::Entity, const fmt::OcSkeleton&, anim::Pose&, void*) { ++g_chained; }

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    AVER_INFO("==================================================");
    AVER_INFO("ragdoll system: a skinned entity as a physics ragdoll");

    std::error_code ec;
    g_dir = (std::filesystem::temp_directory_path(ec) / "aver-ragdoll-system-test").string();
    std::filesystem::remove_all(g_dir, ec);
    std::filesystem::create_directories(g_dir, ec);

    const fmt::OcSkeleton skel = body();
    {
        std::string why;
        check(fmt::saveOcSkel(g_dir + "/body.ocskel", skel, &why), "the skeleton writes: " + why);
        check(fmt::saveOcSkel(g_dir + "/authored.ocskel", skel, &why), "and the authored twin: " + why);
        std::ofstream f(g_dir + "/authored.ocragdoll", std::ios::binary);
        f << "mass 70\nbone Chest shape sphere mass 9 friction 0.75 bounce 0.3\nbone KneeL joint fixed\n";
    }

    check(aver_phys_init() == 1, "physics started");
    aver_phys_set_gravity(0, 0, -980);
    const int32_t floorBody = aver_phys_add_static_box(0, 0, -50, 5000, 5000, 50);   // top face at z = 0
    check(floorBody != 0, "a floor exists");

    scene::World& w = scene::World::instance();
    anim::AnimSystem& sys = anim::animSystem();
    sys.clear();
    sys.setResolver(&resolvePath, nullptr);
    sys.setPoseModifier(&countingModifier, nullptr);

    ragdoll::RagdollSystem& rd = ragdoll::ragdollSystem();
    AVER_INFO("=== installing chains the modifier already there ===");
    {
        check(rd.install(sys, w, &countingModifier, nullptr), "it installs over an existing modifier");
        const scene::Entity e = makeBody(w, "chained", Transform{});
        const int before = g_chained;
        sys.tick(w, 1.0f / 60.0f);
        check(g_chained > before, "the previous modifier still runs");
        check(rd.mode(e) == -1, "an entity never started has no ragdoll");
        check(!rd.start(scene::kInvalidEntity, ragdoll::RagdollMode::Limp, 0.0f), "an invalid entity refuses to start");
        w.destroy(e); w.flush();
    }

    AVER_INFO("=== the pose survives the trip to physics and back, for a moved, turned, scaled entity ===");
    {
        aver_phys_set_gravity(0, 0, 0);
        Transform xf;
        xf.position = Vec3{100, 200, 300};
        xf.rotation = Quat::fromAxisAngle(Vec3{0, 0, 1}, radians(90.0f));
        xf.scale = Vec3{2, 2, 2};
        const scene::Entity e = makeBody(w, "placed", xf);
        sys.tick(w, 1.0f / 60.0f);                         // posed, so start() can build at once

        check(rd.start(e, ragdoll::RagdollMode::Limp, 0.0f), "start succeeds");
        check(rd.physicsHandle(e) != 0, "the physics ragdoll was built in the call");
        check(rd.mode(e) == 1, "in limp mode");
        check(aver_phys_ragdoll_count() == 1, "and it is the only one in the world");
        const ragdoll::RagdollDef* def = rd.definition(e);
        check(def && def->partCount >= 8, "with parts for the limbs and trunk");

        // Every part is a body the world knows by this entity.
        bool owned = true;
        if (def)
            for (u32 b = 0; b < def->boneCount; ++b)
                if (def->hasPart(b)) {
                    const int32_t body = aver_phys_ragdoll_body_of_bone(rd.physicsHandle(e), static_cast<int32_t>(b));
                    int32_t bone = -1;
                    owned = owned && body != 0 && aver_phys_ragdoll_of_body(body, &bone) == rd.physicsHandle(e) &&
                            bone == static_cast<int32_t>(b);
                }
        check(owned, "every part body maps back to its ragdoll and bone");

        // No physics step in between: the pose written back is the pose it was built from.
        ragdoll::ragdollSystem().tick(0.0f);
        sys.tick(w, 0.0f);
        const anim::Pose* p = sys.pose(e);
        check(p != nullptr, "the entity is posed");
        if (p) {
            anim::Pose rest;
            anim::restPose(skel, rest);
            f32 worstPos = 0, worstRot = 1;
            for (usize i = 0; i < rest.local.size(); ++i) {
                worstPos = std::max(worstPos, (p->local[i].position - rest.local[i].position).size());
                const Quat a = p->local[i].rotation, b = rest.local[i].rotation;
                worstRot = std::min(worstRot, std::fabs(a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w));
            }
            check(worstPos < 0.05f, "local positions come back within " + f2s(worstPos) + " cm");
            check(worstRot > 0.9999f, "and local rotations agree, worst dot " + f2s(worstRot));
        }
        rd.stop(e, 0.0f);
        check(rd.mode(e) == -1 && aver_phys_ragdoll_count() == 0, "stop(0) removes it at once");
        w.destroy(e); w.flush();
        aver_phys_set_gravity(0, 0, -980);
    }

    AVER_INFO("=== kinematic: the skin shows the animation ===");
    {
        const scene::Entity e = makeBody(w, "kinematic", Transform{Vec3{0, 0, 400}, Quat::identity(), Vec3{1, 1, 1}});
        check(rd.start(e, ragdoll::RagdollMode::Kinematic, 0.0f), "kinematic starts, before the entity's first pose too");
        frames(w, 0.5f);
        const anim::Pose* p = sys.pose(e);
        const Vec3 hand = p ? boneModel(skel, *p, HandL) : Vec3{};
        check(p && (hand - Vec3{0, -69, 95 + 12 + 18 - 2}).size() < 0.5f,
              "the hand is exactly where the animation puts it, though the parts hang in the air: z " + f2s(hand.z));
        check(rd.weight(e) == 0.0f, "nothing is blended in");
        rd.stop(e, 0.0f);
        w.destroy(e); w.flush();
    }

    AVER_INFO("=== limp: it falls, and lies on the floor ===");
    {
        const scene::Entity e = makeBody(w, "limp", Transform{Vec3{0, 0, 300}, Quat::identity(), Vec3{1, 1, 1}});
        sys.tick(w, 1.0f / 60.0f);
        check(rd.start(e, ragdoll::RagdollMode::Limp, 0.0f), "limp starts");
        frames(w, 0.5f);
        const anim::Pose* p = sys.pose(e);
        check(p && boneModel(skel, *p, Pelvis).z < 95.0f - 50.0f,
              "after half a second the pelvis has dropped, model z " + f2s(p ? boneModel(skel, *p, Pelvis).z : 0));
        frames(w, 4.0f);
        p = sys.pose(e);
        const f32 worldZ = p ? 300.0f + boneModel(skel, *p, Pelvis).z : 0.0f;
        check(p && worldZ > -5.0f && worldZ < 100.0f,
              "and a few seconds on it is resting on the floor, not through it: pelvis at z " + f2s(worldZ));
        rd.stop(e, 0.0f);
        w.destroy(e); w.flush();
    }

    AVER_INFO("=== the blend weight ramps in, and stopping gives the animation back ===");
    {
        const scene::Entity e = makeBody(w, "blend", Transform{Vec3{0, 0, 300}, Quat::identity(), Vec3{1, 1, 1}});
        sys.tick(w, 1.0f / 60.0f);
        check(rd.start(e, ragdoll::RagdollMode::Limp, 0.5f), "limp starts with a half-second blend");
        check(rd.weight(e) == 0.0f, "at weight 0");
        frames(w, 0.25f);
        const f32 half = rd.weight(e);
        check(half > 0.4f && half < 0.6f, "a quarter-second in it is about half, " + f2s(half));
        frames(w, 0.4f);
        check(rd.weight(e) == 1.0f, "and full after the blend");

        check(rd.stop(e, 0.5f), "stop with a blend");
        check(rd.mode(e) == 1, "the ragdoll is still there while it blends out");
        frames(w, 0.25f);
        const f32 mid = rd.weight(e);
        check(mid > 0.4f && mid < 0.6f, "weight is falling, " + f2s(mid));
        frames(w, 0.5f);
        check(rd.mode(e) == -1, "then the ragdoll is gone");
        check(aver_phys_ragdoll_count() == 0, "and so is the physics ragdoll");
        const anim::Pose* p = sys.pose(e);
        anim::Pose rest;
        anim::restPose(skel, rest);
        f32 worst = 0;
        if (p) for (usize i = 0; i < rest.local.size(); ++i)
            worst = std::max(worst, (p->local[i].position - rest.local[i].position).size());
        check(p && worst < 1e-3f, "the pose is the animation again, worst bone off by " + f2s(worst));
        w.destroy(e); w.flush();
    }

    AVER_INFO("=== an impulse moves the part it names ===");
    {
        aver_phys_set_gravity(0, 0, 0);
        const scene::Entity e = makeBody(w, "pushed", Transform{Vec3{0, 0, 300}, Quat::identity(), Vec3{1, 1, 1}});
        sys.tick(w, 1.0f / 60.0f);
        check(rd.start(e, ragdoll::RagdollMode::Limp, 0.0f), "limp starts in zero gravity");
        const u64 head = fnv1a64(std::string_view("Head"));
        check(rd.addImpulse(e, head, Vec3{4000, 0, 0}), "an impulse on the head is accepted");
        check(!rd.addImpulse(e, fnv1a64(std::string_view("NoSuchBone")), Vec3{1, 0, 0}), "an unknown bone is refused");
        check(rd.addImpulse(e, 0, Vec3{1, 0, 0}), "hash 0 addresses the root part");
        frames(w, 0.3f);
        const anim::Pose* p = sys.pose(e);
        const Vec3 headPos = p ? boneModel(skel, *p, Head) : Vec3{};
        check(headPos.x > 10.0f, "the head moved along +X, by " + f2s(headPos.x) + " cm");
        rd.stop(e, 0.0f);
        w.destroy(e); w.flush();
        aver_phys_set_gravity(0, 0, -980);
    }

    AVER_INFO("=== authored per-part data reaches the bodies ===");
    {
        const scene::Entity e = w.create("authored");
        auto* sm = static_cast<scene::CSkeletalMesh*>(w.addComponent(e, scene::kComponentSkeletalMesh));
        w.addComponent(e, scene::kComponentAnimator);
        if (sm) sm->skeleton = kAuthoredId;
        w.setLocalTransform(e, Transform{Vec3{0, 0, 300}, Quat::identity(), Vec3{1, 1, 1}});
        w.flush();
        sys.tick(w, 1.0f / 60.0f);
        check(rd.start(e, ragdoll::RagdollMode::Limp, 0.0f), "the authored body starts");
        const i32 handle = rd.physicsHandle(e);
        const i32 head = aver_phys_ragdoll_body_of_bone(handle, Chest);    // the part with the authored data
        const i32 chest = aver_phys_ragdoll_body_of_bone(handle, Spine);   // and a neighbour without
        float m = 0, fr = 0, re = 0, mc = 0, frc = 0;
        check(head != 0 && aver_phys_body_mass(head, &m) && std::fabs(m - 9.0f) < 1e-3f, "the chest weighs what the file says: " + f2s(m));
        check(aver_phys_body_friction(head, &fr) && std::fabs(fr - 0.75f) < 1e-4f && aver_phys_body_restitution(head, &re) && std::fabs(re - 0.3f) < 1e-4f,
              "with its own friction and bounce");
        check(chest != 0 && aver_phys_body_mass(chest, &mc) && std::fabs(mc - 9.0f) > 0.5f && aver_phys_body_friction(chest, &frc) && std::fabs(frc - 0.75f) > 1e-3f,
              "and the spine is untouched (mass " + f2s(mc) + ")");
        rd.stop(e, 0.0f);
        w.destroy(e); w.flush();
    }

    AVER_INFO("=== a ragdoll does not outlive its entity ===");
    {
        const scene::Entity e = makeBody(w, "doomed", Transform{Vec3{0, 0, 300}, Quat::identity(), Vec3{1, 1, 1}});
        sys.tick(w, 1.0f / 60.0f);
        rd.start(e, ragdoll::RagdollMode::Limp, 0.0f);
        check(aver_phys_ragdoll_count() == 1, "one live ragdoll");
        w.destroy(e); w.flush();
        rd.tick(1.0f / 60.0f);
        check(aver_phys_ragdoll_count() == 0 && rd.activeCount() == 0, "gone with the entity");
    }

    AVER_INFO("=== uninstalling hands the seam back ===");
    {
        rd.uninstall();
        const int before = g_chained;
        const scene::Entity e = makeBody(w, "after", Transform{});
        sys.tick(w, 1.0f / 60.0f);
        check(g_chained > before, "the previous modifier is the installed one again");
        w.destroy(e); w.flush();
    }

    rd.clear();
    sys.clear();
    aver_phys_shutdown();
    std::filesystem::remove_all(g_dir, ec);
    AVER_INFO("==================================================");
    if (g_failures == 0) AVER_INFO("=== {} assertions, 0 failed ===", g_checks);
    else                 AVER_ERROR("=== {} assertions, {} FAILED ===", g_checks, g_failures);
    return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
}
