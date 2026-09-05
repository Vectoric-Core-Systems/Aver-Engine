// ControlRigTest -- a .ocrig on an entity, applied through AnimSystem's pose-modifier seam.
//
// THE WHOLE CHAIN, not a piece of it: a rig FILE on disk, resolved by asset id the way a skeleton
// and a clip are, loaded once, its bone NAMES matched against a real skeleton, its ops run on the
// pose a clip just produced, and the result read back out of the system. Every layer below this has
// its own test; this is the only one that can fail if they are wired together wrongly.
#include "aver/anim/AnimSystem.hpp"
#include "aver/anim/ControlRig.hpp"
#include "aver/anim/Ik.hpp"
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"
#include "aver/formats/OcAnim.hpp"
#include "aver/formats/OcRig.hpp"
#include "aver/scene/Components.hpp"
#include "aver/scene/World.hpp"

#include <cmath>
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

static std::string f2s(f32 v) { char b[64]; std::snprintf(b, sizeof b, "%.2f", v); return b; }
static f32 gap(const Vec3& a, const Vec3& b) { const Vec3 d = a - b; return std::sqrt(dot(d, d)); }

static std::string g_dir;
static const u64 kSkelId = 0x5EE1AA01ull;
static const u64 kRigId  = 0x21600001ull;
static const u64 kMissingRigId = 0x21600002ull;

static std::string resolvePath(u64 id, void*) {
    if (id == kSkelId) return g_dir + "/arm.ocskel";
    if (id == kRigId)  return g_dir + "/reach.ocrig";
    if (id == kMissingRigId) return g_dir + "/gone.ocrig";   // resolves, but no file is there
    return {};
}

// shoulder -> elbow (40 up) -> wrist (60 further up). Reach 100 cm.
static fmt::OcSkeleton armSkeleton() {
    fmt::OcSkeleton s;
    fmt::OcBone shoulder; shoulder.name = "shoulder"; shoulder.parent = -1;
    fmt::OcBone elbow;    elbow.name = "elbow";       elbow.parent = 0; elbow.translation = Vec3{0, 0, 40};
    fmt::OcBone wrist;    wrist.name = "wrist";       wrist.parent = 1; wrist.translation = Vec3{0, 0, 60};
    s.bones = {shoulder, elbow, wrist};
    s.rootBone = 0;
    return s;
}

static Vec3 wristOf(const anim::Pose& p, const fmt::OcSkeleton& s) {
    Vec3 v{0, 0, 0};
    anim::bonePositionModel(s, p, 2, v);
    return v;
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    AVER_INFO("==================================================");
    AVER_INFO("control rig: a .ocrig applied to an entity");

    std::error_code ec;
    g_dir = (std::filesystem::temp_directory_path(ec) / "aver-controlrig-test").string();
    // THE SHIPPED DEMO ASSET, checked against the SHIPPED skeleton it was authored for.
    //
    // A rig naming a bone the skeleton does not have is not an error at any layer -- applyRigOp
    // returns false and the pose is left alone, which is exactly right for a rig written for another
    // character. That correctness is what makes a typo dangerous here: one wrong bone name leaves a
    // file that loads, validates, applies nothing and says nothing. Nothing else in the tree would
    // notice, so this does.
    AVER_INFO("=== the shipped ArmReach.ocrig matches the shipped Character.ocskel ===");
    {
        fmt::OcRigData demo;
        std::string why;
        check(fmt::loadOcRig(AVER_DEMO_RIG, demo, &why), "the demo rig loads: " + why);

        fmt::OcSkeleton character;
        check(fmt::loadOcSkel(AVER_DEMO_SKEL, character, &why), "the character skeleton loads: " + why);

        check(!demo.ops.empty(), "the rig has ops, got " + std::to_string(demo.ops.size()));
        check(character.bones.size() > 20,
              "and the skeleton is the real humanoid, " + std::to_string(character.bones.size()) +
              " bones");

        for (const fmt::OcRigOp& op : demo.ops) {
            u32 idx = 0;
            check(anim::findBone(character, op.root, idx), "bone '" + op.root + "' exists");
            if (op.kind == fmt::OcRigOpKind::TwoBoneIk) {
                check(anim::findBone(character, op.mid, idx), "bone '" + op.mid + "' exists");
                check(anim::findBone(character, op.tip, idx), "bone '" + op.tip + "' exists");

                // AND THEY ARE ACTUALLY A CHAIN. twoBoneIk refuses bones that are not parent-linked,
                // so three real bones in the wrong order is the other way this asset could be inert.
                u32 r = 0, m = 0, t = 0;
                if (anim::findBone(character, op.root, r) && anim::findBone(character, op.mid, m) &&
                    anim::findBone(character, op.tip, t)) {
                    check(character.bones[m].parent == static_cast<i32>(r),
                          "'" + op.mid + "' is a child of '" + op.root + "'");
                    check(character.bones[t].parent == static_cast<i32>(m),
                          "'" + op.tip + "' is a child of '" + op.mid + "'");
                }
            }
        }

        // And it does something: applied to the character's rest pose, the hand moves.
        anim::Pose p;
        anim::restPose(character, p);
        u32 hand = 0;
        if (anim::findBone(character, "LeftHand", hand)) {
            Vec3 before{0, 0, 0};
            anim::bonePositionModel(character, p, hand, before);
            bool applied = false;
            for (const fmt::OcRigOp& op : demo.ops)
                applied = anim::applyRigOp(character, p, op, 1.0f) || applied;
            Vec3 after{0, 0, 0};
            anim::bonePositionModel(character, p, hand, after);
            check(applied, "the rig applies to the character");
            check(gap(before, after) > 5.0f,
                  "and the left hand actually MOVES, by " + f2s(gap(before, after)) + " cm");

            // THE ASSERTION THAT ACTUALLY CHECKS THE ASSET. "it moved" passes for any goal at all,
            // including a nonsense one -- the first version of this rig had a human-scale goal
            // against a skeleton that is 100x oversized (a known importer bug, see the rig's own
            // header), the arm straightened at full stretch toward a point 129 METRES away, and
            // "it moved by 12,919 cm" sailed through. Landing ON the goal is the property that
            // distinguishes a rig authored for this skeleton from one that merely disturbs it.
            for (const fmt::OcRigOp& op : demo.ops) {
                if (op.kind != fmt::OcRigOpKind::TwoBoneIk) continue;
                check(gap(after, op.target) < 1.0f,
                      "and lands ON the rig's goal, missing by " + f2s(gap(after, op.target)) +
                      " cm -- so the goal is inside the arm's reach, not somewhere it can only "
                      "point at");
            }
        }
    }

    std::filesystem::remove_all(g_dir, ec);
    std::filesystem::create_directories(g_dir, ec);

    const fmt::OcSkeleton skel = armSkeleton();
    const Vec3 goal{80, 0, 0};

    {
        std::string why;
        check(fmt::saveOcSkel(g_dir + "/arm.ocskel", skel, &why), "the skeleton writes: " + why);

        fmt::OcRigData rig;
        rig.name = "Reach";
        fmt::OcRigOp op;
        op.kind = fmt::OcRigOpKind::TwoBoneIk;
        op.root = "shoulder"; op.mid = "elbow"; op.tip = "wrist";
        op.target = goal;
        op.hint = Vec3{0, -100, 20};
        op.weight = 1.0f;
        rig.ops.push_back(op);
        check(fmt::saveOcRig(g_dir + "/reach.ocrig", rig, &why), "the rig writes: " + why);
    }

    scene::World& w = scene::World::instance();
    anim::AnimSystem& sys = anim::animSystem();
    sys.clear();
    sys.setResolver(&resolvePath, nullptr);

    AVER_INFO("=== the component registers ===");
    const u32 type = anim::ControlRigSystem::registerComponents(w);
    check(type != 0, "CControlRig got a type id");
    check(w.componentId("CControlRig") == type, "and answers to its name");
    check(w.componentVerified(type),
          "its field table covers the struct with no gap -- a padded one aborts World's ctor");

    anim::ControlRigSystem rigs;
    rigs.install(sys, w);
    check(sys.hasPoseModifier(), "installing the system installs the pose modifier");

    AVER_INFO("=== a rigged entity's wrist is pulled onto the goal ===");
    {
        const scene::Entity e = w.create("rigged");
        auto* sm = static_cast<scene::CSkeletalMesh*>(w.addComponent(e, scene::kComponentSkeletalMesh));
        w.addComponent(e, scene::kComponentAnimator);      // no clip: the rest pose is the input
        check(sm != nullptr, "the entity has a rig component");
        if (sm) sm->skeleton = kSkelId;

        auto* cr = anim::ControlRigSystem::attach(w, e, kRigId);
        check(cr != nullptr, "a control rig attaches");
        // The zero-fill trap, asserted rather than assumed: addComponent runs no constructor, so an
        // attach that forgot to write a fresh value would leave weight at 0 and the rig inert.
        check(cr && cr->weight > 0.0f,
              "and its weight is 1, not the 0 a raw zero-filled attach would leave");

        sys.tick(w, 0.016f);

        const anim::Pose* p = sys.pose(e);
        check(p != nullptr, "the system posed it");
        if (p) {
            const f32 miss = gap(wristOf(*p, skel), goal);
            check(miss < 1.0f,
                  "the wrist is on the rig's goal, missing by " + f2s(miss) +
                  " cm -- with no rig it would be at (0,0,100), 128 cm away");
            check(rigs.appliedLastTick() > 0, "and the system counted the entity it applied to");
        }
        w.destroy(e); w.flush();
    }

    AVER_INFO("=== weight decides how much of it lands ===");
    {
        const auto wristAtWeight = [&](f32 weight) {
            const scene::Entity e = w.create("weighted");
            auto* sm = static_cast<scene::CSkeletalMesh*>(w.addComponent(e, scene::kComponentSkeletalMesh));
            w.addComponent(e, scene::kComponentAnimator);
            if (sm) sm->skeleton = kSkelId;
            anim::ControlRigSystem::attach(w, e, kRigId, weight);
            sys.tick(w, 0.016f);
            const anim::Pose* p = sys.pose(e);
            const Vec3 v = p ? wristOf(*p, skel) : Vec3{0, 0, 0};
            w.destroy(e); w.flush();
            return v;
        };

        const Vec3 rest{0, 0, 100};
        const Vec3 off  = wristAtWeight(0.0f);
        const Vec3 half = wristAtWeight(0.5f);
        const Vec3 full = wristAtWeight(1.0f);

        check(gap(off, rest) < 1.0f,
              "weight 0 leaves the sampled pose untouched, wrist still at " + f2s(off.z) + " on z");
        check(gap(full, goal) < 1.0f, "weight 1 reaches the goal");
        // Between the two, and genuinely between -- not snapped to either end, which is what a
        // weight implemented as a bool would produce.
        check(gap(half, rest) > 5.0f && gap(half, goal) > 5.0f,
              "weight 0.5 is PART WAY, " + f2s(gap(half, rest)) + " cm from rest and " +
              f2s(gap(half, goal)) + " cm from the goal");
    }

    AVER_INFO("=== a rig that cannot be loaded is survivable ===");
    {
        const scene::Entity e = w.create("missingRig");
        auto* sm = static_cast<scene::CSkeletalMesh*>(w.addComponent(e, scene::kComponentSkeletalMesh));
        w.addComponent(e, scene::kComponentAnimator);
        if (sm) sm->skeleton = kSkelId;
        anim::ControlRigSystem::attach(w, e, kMissingRigId);
        sys.tick(w, 0.016f);
        sys.tick(w, 0.016f);            // twice: the failure must be cached, not re-reported
        const anim::Pose* p = sys.pose(e);
        check(p != nullptr, "the entity still poses");
        if (p) check(gap(wristOf(*p, skel), Vec3{0, 0, 100}) < 1.0f,
                     "at its sampled pose, unmodified");
        w.destroy(e); w.flush();
    }

    AVER_INFO("=== a rig whose bones do not exist on this skeleton does nothing ===");
    {
        // The ordinary case for a rig authored against a different character -- not an error.
        fmt::OcRigData rig;
        fmt::OcRigOp op;
        op.root = "thigh"; op.mid = "knee"; op.tip = "ankle";
        op.target = goal; op.hint = Vec3{0, -100, 0};
        rig.ops.push_back(op);

        anim::Pose p;
        anim::restPose(skel, p);
        const Vec3 before = wristOf(p, skel);
        check(!anim::applyRigOp(skel, p, rig.ops[0], 1.0f), "the op reports it did not apply");
        check(gap(wristOf(p, skel), before) < 1e-3f, "and the pose is untouched");
    }

    AVER_INFO("=== uninstalling restores the un-rigged tick exactly ===");
    {
        anim::ControlRigSystem::uninstall(sys);
        check(!sys.hasPoseModifier(), "the modifier is gone");
        const scene::Entity e = w.create("afterUninstall");
        auto* sm = static_cast<scene::CSkeletalMesh*>(w.addComponent(e, scene::kComponentSkeletalMesh));
        w.addComponent(e, scene::kComponentAnimator);
        if (sm) sm->skeleton = kSkelId;
        anim::ControlRigSystem::attach(w, e, kRigId);
        sys.tick(w, 0.016f);
        const anim::Pose* p = sys.pose(e);
        check(p && gap(wristOf(*p, skel), Vec3{0, 0, 100}) < 1.0f,
              "and an entity WITH a rig component is left at its sampled pose");
        w.destroy(e); w.flush();
    }

    std::filesystem::remove_all(g_dir, ec);
    AVER_INFO("==================================================");
    if (g_failures == 0) AVER_INFO("=== {} assertions, 0 failed ===", g_checks);
    else                 AVER_ERROR("=== {} assertions, {} FAILED ===", g_checks, g_failures);
    return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
}
