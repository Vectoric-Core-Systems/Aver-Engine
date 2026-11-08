// PhysicalAnimationTest -- the physical-animation layer on a real (CPU, headless) Jolt ragdoll: per-part drive
// and pinning in the ABI, partial-body simulation, hit reactions that blend in and hand the body back to the
// animation by themselves, whole-body blend in and out, and death / get-up. A small skeleton, no GPU.
#include "aver/anim/AnimSystem.hpp"
#include "aver/anim/Pose.hpp"
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Hash.hpp"
#include "aver/core/HitEvent.hpp"
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
#include <memory>
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
static const u64 kSkelId = 0x4A6D0211ull;

static std::string resolvePath(u64 id, void*) { return id == kSkelId ? g_dir + "/body.ocskel" : std::string{}; }

enum Bone : int { Root, Pelvis, Spine, Chest, Neck, Head, ThighL, KneeL, FootL, UpperArmL, ForearmL, HandL, BoneCount };

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

static const fmt::OcSkeleton g_skel = body();
static const f32 kBaseZ = 300.0f;

static scene::Entity makeBody(scene::World& w, const char* name) {
    const scene::Entity e = w.create(name);
    auto* sm = static_cast<scene::CSkeletalMesh*>(w.addComponent(e, scene::kComponentSkeletalMesh));
    w.addComponent(e, scene::kComponentAnimator);
    if (sm) sm->skeleton = kSkelId;
    w.setLocalTransform(e, Transform{Vec3{0, 0, kBaseZ}, Quat::identity(), Vec3{1, 1, 1}});
    w.flush();
    anim::animSystem().tick(w, 1.0f / 60.0f);   // posed, so the ragdoll can be built at once
    return e;
}

static Vec3 modelPos(const anim::Pose& p, int bone) {
    std::vector<Mat4> model;
    anim::poseToModel(g_skel, p, model);
    return {model[static_cast<usize>(bone)].m[3][0], model[static_cast<usize>(bone)].m[3][1], model[static_cast<usize>(bone)].m[3][2]};
}

static Vec3 restModel(int bone) {
    anim::Pose rest;
    anim::restPose(g_skel, rest);
    return modelPos(rest, bone);
}

static Vec3 poseBone(scene::Entity e, int bone) {
    const anim::Pose* p = anim::animSystem().pose(e);
    return p ? modelPos(*p, bone) : Vec3{1e9f, 1e9f, 1e9f};
}

// The physics part of a bone, in the entity's model space (the entity sits at (0,0,kBaseZ), unrotated).
static Vec3 physicsBone(int handle, int bone) {
    std::vector<f32> pos(BoneCount * 3), rot(BoneCount * 4);
    aver_phys_ragdoll_get_pose(handle, pos.data(), rot.data());
    return {pos[static_cast<usize>(bone) * 3], pos[static_cast<usize>(bone) * 3 + 1], pos[static_cast<usize>(bone) * 3 + 2] - kBaseZ};
}

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

// How far a bone's local rotation is from the rest pose: 1 - |dot|, 0 when it is the animation.
static f32 localDeviation(scene::Entity e, int bone) {
    const anim::Pose* p = anim::animSystem().pose(e);
    anim::Pose rest;
    anim::restPose(g_skel, rest);
    if (!p) return -1.0f;
    const Quat a = p->local[static_cast<usize>(bone)].rotation, b = rest.local[static_cast<usize>(bone)].rotation;
    return 1.0f - std::fabs(a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w);
}

static u64 H(const char* n) { return fnv1a64(std::string_view(n)); }

static u64 g_getUpMontage = 0;
static f32 g_getUpBlend = -1;
static scene::Entity g_getUpEntity = 0;
static void getUpHandler(scene::Entity e, u64 montage, f32 blend, void*) { g_getUpEntity = e; g_getUpMontage = montage; g_getUpBlend = blend; }

// Rotates every bone of a ragdoll's physics pose about the pelvis: how a fallen body is staged without the chaos of a fall.
static void rotatePhysicsPose(int handle, const Quat& q) {
    std::vector<f32> pos(BoneCount * 3), rot(BoneCount * 4);
    aver_phys_ragdoll_get_pose(handle, pos.data(), rot.data());
    const Vec3 pivot{pos[Pelvis * 3], pos[Pelvis * 3 + 1], pos[Pelvis * 3 + 2]};
    for (int b = 0; b < BoneCount; ++b) {
        const Vec3 p{pos[static_cast<usize>(b) * 3], pos[static_cast<usize>(b) * 3 + 1], pos[static_cast<usize>(b) * 3 + 2]};
        const Vec3 n = pivot + q.rotate(p - pivot);
        pos[static_cast<usize>(b) * 3] = n.x; pos[static_cast<usize>(b) * 3 + 1] = n.y; pos[static_cast<usize>(b) * 3 + 2] = n.z;
        const Quat r = (q * Quat{rot[static_cast<usize>(b) * 4], rot[static_cast<usize>(b) * 4 + 1], rot[static_cast<usize>(b) * 4 + 2], rot[static_cast<usize>(b) * 4 + 3]}).normalized();
        rot[static_cast<usize>(b) * 4] = r.x; rot[static_cast<usize>(b) * 4 + 1] = r.y; rot[static_cast<usize>(b) * 4 + 2] = r.z; rot[static_cast<usize>(b) * 4 + 3] = r.w;
    }
    aver_phys_ragdoll_set_pose(handle, pos.data(), rot.data());
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    AVER_INFO("==================================================");
    AVER_INFO("physical animation on a headless Jolt ragdoll");

    std::error_code ec;
    g_dir = (std::filesystem::temp_directory_path(ec) / "aver-physanim-system-test").string();
    std::filesystem::remove_all(g_dir, ec);
    std::filesystem::create_directories(g_dir, ec);
    {
        std::string why;
        check(fmt::saveOcSkel(g_dir + "/body.ocskel", g_skel, &why), "the skeleton writes: " + why);
    }

    check(aver_phys_init() == 1, "physics started");
    aver_phys_set_gravity(0, 0, 0);

    scene::World& w = scene::World::instance();
    anim::AnimSystem& sys = anim::animSystem();
    sys.clear();
    sys.setResolver(&resolvePath, nullptr);
    ragdoll::RagdollSystem& rd = ragdoll::ragdollSystem();
    check(rd.install(sys, w), "the ragdoll system installs");

    AVER_INFO("=== the ABI: per part drive and pins ===");
    {
        const scene::Entity e = makeBody(w, "abi");
        check(rd.start(e, ragdoll::RagdollMode::Powered, 0.0f), "a powered ragdoll starts");
        const int h = rd.physicsHandle(e);
        check(aver_phys_ragdoll_set_part_strength(h, UpperArmL, 0.9f) == 1 && aver_phys_ragdoll_set_part_damping(h, UpperArmL, 2.0f) == 1 &&
                  aver_phys_ragdoll_set_part_torque_scale(h, UpperArmL, 3.0f) == 1, "per part strength, damping and torque scale are accepted");
        check(aver_phys_ragdoll_set_part_strength(h, UpperArmL, -1.0f) == 1, "a negative strength goes back to inherit");
        check(aver_phys_ragdoll_set_part_strength(9999, UpperArmL, 0.5f) == 0, "a dead handle is refused");
        check(aver_phys_ragdoll_set_part_strength(h, 99, 0.5f) == 0 && aver_phys_ragdoll_set_part_strength(h, -1, 0.5f) == 0, "a bad bone is refused");
        check(aver_phys_ragdoll_set_part_strength(h, UpperArmL, NAN) == 0, "a non-finite value is refused");
        check(aver_phys_ragdoll_part_pinned(h, Chest) == 0, "nothing is pinned by default");
        check(aver_phys_ragdoll_set_part_pinned(h, Chest, 1) == 1 && aver_phys_ragdoll_part_pinned(h, Chest) == 1, "a part can be pinned");
        check(aver_phys_ragdoll_part_pinned(h, 99) == -1, "pinned? of a bad bone is -1");
        rd.stop(e, 0.0f);
        w.destroy(e); w.flush();
    }

    AVER_INFO("=== partial-body simulation: only the arm is simulated ===");
    {
        aver_phys_set_gravity(0, 0, -980);
        const scene::Entity e = makeBody(w, "partial");
        check(rd.start(e, ragdoll::RagdollMode::Powered, 0.0f), "powered starts");
        check(rd.simulateSubtree(e, H("UpperArmL")), "the left arm is the simulated subtree");
        check(!rd.simulateSubtree(e, H("NoSuchBone")), "an unknown bone is refused");
        const int h = rd.physicsHandle(e);
        check(aver_phys_ragdoll_part_pinned(h, Chest) == 1 && aver_phys_ragdoll_part_pinned(h, Head) == 1 &&
                  aver_phys_ragdoll_part_pinned(h, UpperArmL) == 0 && aver_phys_ragdoll_part_pinned(h, HandL) == 0,
              "everything outside the subtree is pinned, the arm is not");
        check(rd.boneBlend(e, Chest) == 0.0f && rd.boneBlend(e, HandL) == 1.0f, "and the pinned bones show no simulated pose");

        // A weak arm sags under gravity; the rest of the body stays where the animation puts it.
        rd.setPartStrength(e, ForearmL, 0.0f);
        rd.setPartStrength(e, UpperArmL, 0.0f);
        rd.setPartStrength(e, HandL, 0.0f);
        frames(w, 1.5f);
        const Vec3 handRest = restModel(HandL);
        const f32 sagWeak = handRest.z - physicsBone(h, HandL).z;
        check(sagWeak > 12.0f, "a weak arm sags: the hand is " + f2s(sagWeak) + " cm below the animation");
        f32 worstOutside = 0;
        for (int b : {Pelvis, Spine, Chest, Neck, Head, ThighL, KneeL, FootL})
            worstOutside = std::max(worstOutside, (physicsBone(h, b) - restModel(b)).size());
        check(worstOutside < 1.0f, "the parts outside the region follow the animation: worst " + f2s(worstOutside) + " cm");
        check(std::fabs(poseBone(e, Head).z - restModel(Head).z) < 0.5f, "and the pose shows the animation there");

        // A strong arm holds the pose (or climbs back to it).
        for (int b : {UpperArmL, ForearmL, HandL}) rd.setPartStrength(e, b, 1.0f);
        frames(w, 2.0f);
        const f32 sagStrong = handRest.z - physicsBone(h, HandL).z;
        check(sagStrong < sagWeak * 0.5f, "a strong arm holds much better, sag " + f2s(sagStrong) + " cm against " + f2s(sagWeak));

        check(rd.simulateSubtree(e, 0) && aver_phys_ragdoll_part_pinned(h, Chest) == 0, "hash 0 simulates the whole body again");
        rd.stop(e, 0.0f);
        w.destroy(e); w.flush();
        aver_phys_set_gravity(0, 0, 0);
    }

    AVER_INFO("=== per bone blend ===");
    {
        const scene::Entity a = makeBody(w, "blendA");
        rd.start(a, ragdoll::RagdollMode::Limp, 0.0f);
        rd.addImpulse(a, H("Head"), Vec3{4000, 0, 0});
        frames(w, 0.3f);
        const f32 moved = localDeviation(a, Head) + localDeviation(a, Neck);
        rd.stop(a, 0.0f);
        w.destroy(a); w.flush();

        const scene::Entity b = makeBody(w, "blendB");
        rd.start(b, ragdoll::RagdollMode::Limp, 0.0f);
        check(rd.setBoneBlend(b, Head, 0.0f) && rd.setBoneBlend(b, Neck, 0.0f), "head and neck are held to the animation");
        check(!rd.setBoneBlend(b, 99, 0.0f), "a bad bone is refused");
        rd.addImpulse(b, H("Head"), Vec3{4000, 0, 0});
        frames(w, 0.3f);
        const f32 held = localDeviation(b, Head) + localDeviation(b, Neck);
        check(moved > 1e-3f && held < 1e-6f, "the same hit bends head and neck by " + f2s(moved * 1000.0f) + " milli-units, or " + f2s(held * 1000.0f) + " with the bones blended out");
        rd.stop(b, 0.0f);
        w.destroy(b); w.flush();
    }

    AVER_INFO("=== a hit reaction blends in and hands the body back by itself ===");
    {
        const scene::Entity e = makeBody(w, "hit");
        check(rd.life(e) == ragdoll::Life::Alive && rd.mode(e) == -1, "an untouched entity has no ragdoll and is alive");
        HitEvent hit;
        hit.entity = e;
        hit.boneHash = H("Head");
        hit.impulse = Vec3{3500, 0, 0};
        hit.damage = 10.0f;
        check(rd.applyHit(hit), "the hit is accepted");
        check(rd.mode(e) == 2 && rd.hitReaction(e) != nullptr, "a powered ragdoll was started for the reaction");
        const int h = rd.physicsHandle(e);
        check(aver_phys_ragdoll_part_pinned(h, Head) == 0 && aver_phys_ragdoll_part_pinned(h, Neck) == 0, "the hit region is simulated");
        check(aver_phys_ragdoll_part_pinned(h, ThighL) == 1 && aver_phys_ragdoll_part_pinned(h, FootL) == 1, "bones further than the reach stay pinned to the animation");

        f32 peakHead = 0, peakBlend = 0, peakNeck = 0;
        for (int i = 0; i < 30; ++i) {
            frame(w, 1.0f / 60.0f);
            peakHead = std::max(peakHead, (poseBone(e, Head) - restModel(Head)).size());
            peakBlend = std::max(peakBlend, rd.boneBlend(e, Head));
            peakNeck = std::max(peakNeck, rd.boneBlend(e, Neck));
        }
        check(peakHead > 1.5f, "the head is knocked out of the animated pose, by " + f2s(peakHead) + " cm");
        check(peakBlend > 0.9f && peakNeck > 0.4f && peakNeck < peakBlend, "the head blends in fully and the neck less: " + f2s(peakBlend) + " / " + f2s(peakNeck));
        check(rd.boneBlend(e, FootL) == 0.0f, "and the far foot not at all");

        // A second hit on a ragdoll that is reacting stacks; it does not start another.
        check(rd.applyHit(hit) && aver_phys_ragdoll_count() == 1, "a second hit reuses the reacting ragdoll");

        frames(w, 2.0f);
        check(rd.mode(e) == -1 && aver_phys_ragdoll_count() == 0, "when the envelopes have run out the ragdoll is gone by itself");
        const anim::Pose* p = sys.pose(e);
        f32 worst = 0;
        if (p) for (int b = 0; b < BoneCount; ++b) worst = std::max(worst, (modelPos(*p, b) - restModel(b)).size());
        check(p && worst < 1e-2f, "and the pose is the animation again, worst " + f2s(worst) + " cm");

        // A hit nobody can react to.
        HitEvent bad = hit;
        bad.boneHash = H("NoSuchBone");
        check(!rd.applyHit(bad) && aver_phys_ragdoll_count() == 0, "an unknown bone reacts to nothing and leaves no ragdoll behind");
        bad.entity = 0xFFFFFFu;
        check(!rd.applyHit(bad), "and neither does an entity that does not exist");
        w.destroy(e); w.flush();
    }

    AVER_INFO("=== a profile changes how it reacts ===");
    {
        auto prof = std::make_shared<ragdoll::PhysAnimProfile>();
        prof->maxHops = 0;
        prof->regions.push_back(ragdoll::PhysAnimRegion{"head", {"Head"}, {}, {}});
        prof->regions[0].drive.blend = 0.5f;
        const scene::Entity e = makeBody(w, "profiled");
        rd.setProfile(e, prof);
        HitEvent hit;
        hit.entity = e;
        hit.boneHash = H("Head");
        hit.impulse = Vec3{3500, 0, 0};
        check(rd.applyHit(hit), "the hit is accepted");
        frames(w, 0.1f);
        check(rd.boneBlend(e, Neck) == 0.0f && rd.boneBlend(e, Head) > 0.2f && rd.boneBlend(e, Head) <= 0.5f + 1e-4f,
              "reach 0 hops keeps it on the head, and the region caps its blend at 0.5: " + f2s(rd.boneBlend(e, Head)));
        rd.stop(e, 0.0f);
        w.destroy(e); w.flush();
    }

    AVER_INFO("=== a radial event reacts what is inside its reach ===");
    {
        const scene::Entity e = makeBody(w, "blast");
        HitEvent blast;
        blast.entity = e;
        blast.point = Vec3{40, 0, kBaseZ + restModel(Chest).z};   // in front of the chest
        blast.radius = 80.0f;
        blast.impulse = Vec3{3000, 0, 0};
        check(rd.applyHit(blast), "the blast is accepted");
        frames(w, 0.05f);
        check(rd.boneBlend(e, Chest) > 0.0f && rd.boneBlend(e, FootL) == 0.0f, "the chest reacts and the foot, far out of reach, does not");
        rd.stop(e, 0.0f);
        w.destroy(e); w.flush();
    }

    AVER_INFO("=== a caller-owned ragdoll only receives the impulse ===");
    {
        const scene::Entity e = makeBody(w, "owned");
        rd.start(e, ragdoll::RagdollMode::Limp, 0.0f);
        HitEvent hit;
        hit.entity = e;
        hit.boneHash = H("Head");
        hit.impulse = Vec3{4000, 0, 0};
        check(rd.applyHit(hit) && rd.hitReaction(e) == nullptr, "the hit is applied as an impulse, with no reaction state");
        frames(w, 0.3f);
        check(poseBone(e, Head).x > 10.0f, "the head moved, " + f2s(poseBone(e, Head).x) + " cm");
        rd.stop(e, 0.0f);
        w.destroy(e); w.flush();
    }

    AVER_INFO("=== whole-body physical animation weight ===");
    {
        const scene::Entity e = makeBody(w, "weight");
        check(rd.blendPhysicalAnimation(e, 1.0f, 0.5f), "blending in starts a powered ragdoll");
        check(rd.mode(e) == 2 && rd.weight(e) == 0.0f, "at weight 0");
        frames(w, 0.25f);
        check(rd.weight(e) > 0.4f && rd.weight(e) < 0.6f, "half way after a quarter second: " + f2s(rd.weight(e)));
        frames(w, 0.4f);
        check(rd.weight(e) == 1.0f, "full after the blend");
        check(rd.blendPhysicalAnimation(e, 0.0f, 0.2f), "blending out");
        frames(w, 0.3f);
        check(rd.weight(e) == 0.0f && rd.mode(e) == 2, "back to the animation, the ragdoll kept for the next blend");
        rd.stop(e, 0.0f);
        check(!rd.blendPhysicalAnimation(0xFFFFFFu, 1.0f, 0.1f), "a missing entity is refused");
        w.destroy(e); w.flush();
    }

    AVER_INFO("=== death and getting up ===");
    {
        rd.setGetUpHandler(&getUpHandler, nullptr);
        auto prof = std::make_shared<ragdoll::PhysAnimProfile>();
        prof->deathSeconds = 0.4f;
        prof->getUpBlend = 0.3f;
        prof->getUpSupine = 11;
        prof->getUpProne = 22;
        prof->getUpSide = 33;

        const scene::Entity e = makeBody(w, "mortal");
        rd.setProfile(e, prof);
        check(!rd.getUp(e), "an entity with no ragdoll cannot get up");
        check(rd.die(e), "it dies");
        check(rd.life(e) == ragdoll::Life::Dying && rd.mode(e) == 2, "dying, still powered");
        check(!rd.die(e), "and cannot die again");
        frames(w, 0.2f);
        check(rd.life(e) == ragdoll::Life::Dying, "still dying half way");
        check(!rd.getUp(e), "it cannot get up while dying");
        frames(w, 0.3f);
        check(rd.life(e) == ragdoll::Life::Dead && rd.mode(e) == 1, "dead means limp");
        check(rd.facing(e) == ragdoll::Facing::Upright, "it has not moved from where it stands");

        // Stage three ways to lie, from the pose the body died in.
        const int h = rd.physicsHandle(e);
        const Quat aboutY = Quat::fromAxisAngle(Vec3{0, 1, 0}, radians(90.0f));
        const Quat aboutYBack = Quat::fromAxisAngle(Vec3{0, 1, 0}, radians(-90.0f));
        rotatePhysicsPose(h, aboutY);
        const ragdoll::Facing f1 = rd.facing(e);
        rotatePhysicsPose(h, aboutY);   // 180 in all: upside down is not "lying", so go back and stage the other way
        rotatePhysicsPose(h, aboutYBack);
        rotatePhysicsPose(h, aboutYBack);
        rotatePhysicsPose(h, aboutYBack);
        const ragdoll::Facing f2 = rd.facing(e);
        check((f1 == ragdoll::Facing::Supine && f2 == ragdoll::Facing::Prone) || (f1 == ragdoll::Facing::Prone && f2 == ragdoll::Facing::Supine),
              "a quarter turn each way lies the body supine one way and prone the other");
        rotatePhysicsPose(h, aboutY);   // upright again
        rotatePhysicsPose(h, Quat::fromAxisAngle(Vec3{1, 0, 0}, radians(90.0f)));
        check(rd.facing(e) == ragdoll::Facing::Side, "a quarter turn about the facing axis lies it on its side");

        rotatePhysicsPose(h, Quat::fromAxisAngle(Vec3{1, 0, 0}, radians(-90.0f)));
        rotatePhysicsPose(h, f1 == ragdoll::Facing::Supine ? aboutY : aboutYBack);   // supine again
        const ragdoll::Facing lying = rd.facing(e);
        check(lying == ragdoll::Facing::Supine, "staged supine");
        g_getUpMontage = 0;
        check(rd.getUp(e), "get up from dead");
        check(g_getUpEntity == e && g_getUpMontage == 11 && std::fabs(g_getUpBlend - 0.3f) < 1e-5f, "the supine montage is played with the profile's blend: " + std::to_string(g_getUpMontage));
        check(rd.life(e) == ragdoll::Life::GettingUp, "getting up");
        check(!rd.getUp(e), "and cannot do it twice");
        frames(w, 0.5f);
        check(rd.life(e) == ragdoll::Life::Alive && rd.mode(e) == -1 && aver_phys_ragdoll_count() == 0, "after the blend the character is alive and the ragdoll is gone");
        rd.setGetUpHandler(nullptr, nullptr);
        w.destroy(e); w.flush();
    }

    rd.uninstall();
    rd.clear();
    sys.clear();
    aver_phys_shutdown();
    std::filesystem::remove_all(g_dir, ec);
    AVER_INFO("==================================================");
    if (g_failures == 0) AVER_INFO("=== {} assertions, 0 failed ===", g_checks);
    else                 AVER_ERROR("=== {} assertions, {} FAILED ===", g_checks, g_failures);
    return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
}
