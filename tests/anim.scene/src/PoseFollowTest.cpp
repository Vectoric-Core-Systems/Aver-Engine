// PoseFollowTest -- pose-to-pose retarget on entities: one entity takes another entity's pose across a .ocretarget every tick.
// The source is posed through the pose seam (a script bone pose); the follower, a different skeleton, must show the same arm
// direction. Also the weight blend, a missing source, the chain with a pose source already installed, and the rest of the refusals.
// No GPU.
#include "../../anim/src/RetargetFixture.hpp"

#include "aver/anim/AnimSystem.hpp"
#include "aver/anim/ControlRig.hpp"
#include "aver/anim/PoseFollow.hpp"
#include "aver/anim/PoseSeam.hpp"
#include "aver/anim/Retarget.hpp"
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Hash.hpp"
#include "aver/core/Log.hpp"
#include "aver/scene/Components.hpp"
#include "aver/scene/World.hpp"

#include <cmath>
#include <filesystem>
#include <map>
#include <string>

using namespace aver;
using namespace rtfix;

static int g_checks = 0, g_failures = 0;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { AVER_INFO("   ok    {}", what); return; }
    AVER_ERROR("   FAIL  {}", what);
    ++g_failures;
}

static std::string f2s(f32 v) { char b[64]; std::snprintf(b, sizeof b, "%.3f", v); return b; }

static std::string g_dir;
static std::map<u64, std::string> g_paths;

static u64 reg(const std::string& ref, const std::string& file) {
    const u64 id = fnv1a64(std::string_view(ref));
    g_paths[id] = g_dir + "/" + file;
    return id;
}
static std::string resolvePath(u64 id, void*) {
    const auto it = g_paths.find(id);
    return it == g_paths.end() ? std::string{} : it->second;
}

static scene::Entity makeRigged(scene::World& w, const char* name, u64 skeleton) {
    const scene::Entity e = w.create(name);
    auto* sm = static_cast<scene::CSkeletalMesh*>(w.addComponent(e, scene::kComponentSkeletalMesh));
    w.addComponent(e, scene::kComponentAnimator);
    if (sm) sm->skeleton = skeleton;
    return e;
}

static int g_inner = 0;
static bool innerSource(scene::Entity, const fmt::OcSkeleton&, anim::Pose&, void*) { ++g_inner; return false; }

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    AVER_INFO("==================================================");
    AVER_INFO("pose follow: a pose carried across a bone map");

    std::error_code ec;
    g_dir = (std::filesystem::temp_directory_path(ec) / "aver-posefollow-test").string();
    std::filesystem::remove_all(g_dir, ec);
    std::filesystem::create_directories(g_dir, ec);
    std::string why;

    const Rig mannequin = makeRig(Params{Naming::Ue, 1.0f, false, 3, true});
    const Rig hero = makeRig(Params{Naming::Ue, 1.0f, true, 4, false});
    const u64 mannequinId = reg("Characters/mannequin.ocskel", "mannequin.ocskel");
    const u64 heroId = reg("Characters/hero.ocskel", "hero.ocskel");
    check(fmt::saveOcSkel(g_dir + "/mannequin.ocskel", mannequin.skel, &why) && fmt::saveOcSkel(g_dir + "/hero.ocskel", hero.skel, &why),
          "(skeletons written) " + why);
    anim::RetargetAsset map = anim::autoMapChains(mannequin.skel, hero.skel).asset;
    map.name = "Mannequin_to_Hero";
    map.source = "Characters/mannequin.ocskel";
    map.target = "Characters/hero.ocskel";
    check(anim::saveRetarget(g_dir + "/good.ocretarget", map, &why), "(bone map written) " + why);
    const u64 mapId = reg("Characters/good.ocretarget", "good.ocretarget");
    anim::RetargetAsset bad = map;
    bad.chains[0].srcStart = "no_such_bone";
    check(anim::saveRetarget(g_dir + "/bad.ocretarget", bad, &why), "(broken bone map written) " + why);
    const u64 badId = reg("Characters/bad.ocretarget", "bad.ocretarget");

    scene::World& w = scene::World::instance();
    anim::AnimSystem& sys = anim::animSystem();
    sys.clear();
    sys.setResolver(&resolvePath, nullptr);
    sys.setPoseSource(&innerSource, nullptr);

    AVER_INFO("=== registration ===");
    anim::ControlRigSystem rigs;
    rigs.registerComponents(w);
    rigs.install(sys, w);
    const u32 type = anim::PoseFollowSystem::registerComponent(w);
    check(type != 0 && w.componentId("CPoseFollow") == type && w.componentVerified(type), "CPoseFollow registers with a verified field table");
    anim::PoseFollowSystem follow;
    follow.install(sys, w);
    follow.install(sys, w);   // idempotent: chaining itself would call itself
    const int innerBefore = g_inner;

    AVER_INFO("=== the follower shows the source's arm ===");
    const scene::Entity src = makeRigged(w, "source", mannequinId);
    const scene::Entity dst = makeRigged(w, "follower", heroId);
    w.flush();
    sys.tick(w, 0.016f);
    check(g_inner > innerBefore, "the pose source installed before it still runs for entities that do not follow");
    const anim::Pose* restHero = sys.pose(dst);
    const Vec3 restDir = restHero ? dirModel(hero.skel, *restHero, hero.upper[0], hero.lower[0]) : Vec3{};

    // Raise the source's left arm with a script bone pose.
    anim::Pose raised;
    anim::restPose(mannequin.skel, raised);
    rotateBone(mannequin.skel, raised, mannequin.upper[0], Quat::fromAxisAngle(Vec3{1, 0, 0}, -radians(90.0f)));
    anim::PoseSeamSystem& seam = rigs.poseSeam();
    check(seam.setBone(src, static_cast<u32>(mannequin.upper[0]), raised.local[static_cast<usize>(mannequin.upper[0])], anim::kPoseSeamRotation, 1.0f),
          "the source's arm is raised through the pose seam");

    anim::CPoseFollow* c = anim::PoseFollowSystem::attach(w, dst, mapId, src, 1.0f);
    check(c != nullptr && c->weight == 1.0f && c->source == src, "the follower attaches with weight 1 (not the 0 a raw attach leaves)");
    sys.tick(w, 0.016f);
    sys.tick(w, 0.016f);

    // What the retargeter itself says for the source's final pose.
    anim::Retargeter reference;
    check(reference.bind(map, mannequin.skel, hero.skel), "(reference retargeter binds)");
    anim::Pose srcFinal = *sys.pose(src);
    anim::Pose expected;
    reference.retarget(srcFinal, expected);
    const anim::Pose* got = sys.pose(dst);
    check(got != nullptr && follow.appliedLastTick() >= 1, "the follower was posed from the source");
    if (got) {
        const Vec3 d = dirModel(hero.skel, *got, hero.upper[0], hero.lower[0]);
        const Vec3 e = dirModel(hero.skel, expected, hero.upper[0], hero.lower[0]);
        check((d - e).size() < 1e-3f, "its arm points where the retargeter puts it: " + f2s(d.x) + " " + f2s(d.y) + " " + f2s(d.z));
        check((d - restDir).size() > 0.3f, "and not where the rest pose has it");
        check(d.z > 0.9f, "straight up, like the source's raised arm, z " + f2s(d.z));
    }

    AVER_INFO("=== weight blends with the pose underneath ===");
    {
        c = w.component<anim::CPoseFollow>(dst, type);
        c->weight = 0.5f;
        sys.tick(w, 0.016f);
        const anim::Pose* p = sys.pose(dst);
        const Vec3 half = p ? dirModel(hero.skel, *p, hero.upper[0], hero.lower[0]) : Vec3{};
        check(half.z > restDir.z + 0.05f && half.z < 0.95f, "half weight is between the rest and the full retarget, z " + f2s(half.z));
        c->weight = 0.0f;
        sys.tick(w, 0.016f);
        const anim::Pose* off = sys.pose(dst);
        const Vec3 d0 = off ? dirModel(hero.skel, *off, hero.upper[0], hero.lower[0]) : Vec3{};
        check((d0 - restDir).size() < 1e-3f, "weight 0 follows nothing");
        c->weight = 1.0f;
    }

    AVER_INFO("=== refusals ===");
    {
        c = w.component<anim::CPoseFollow>(dst, type);
        const scene::Entity ghost = w.create("ghost");   // no skeleton
        c->source = ghost;
        sys.tick(w, 0.016f);
        const anim::Pose* p = sys.pose(dst);
        const Vec3 d = p ? dirModel(hero.skel, *p, hero.upper[0], hero.lower[0]) : Vec3{};
        check((d - restDir).size() < 1e-3f, "a source with no skeleton leaves the follower on its own pose");
        c->source = dst;
        sys.tick(w, 0.016f);
        check(sys.pose(dst) != nullptr, "following itself is ignored");
        c->source = src;
        c->asset = badId;
        sys.tick(w, 0.016f);
        p = sys.pose(dst);
        const Vec3 d2 = p ? dirModel(hero.skel, *p, hero.upper[0], hero.lower[0]) : Vec3{};
        check((d2 - restDir).size() < 1e-3f, "a bone map that does not bind leaves the follower on its own pose");
        c->asset = mapId;
        sys.tick(w, 0.016f);
        p = sys.pose(dst);
        const Vec3 d3 = p ? dirModel(hero.skel, *p, hero.upper[0], hero.lower[0]) : Vec3{};
        check(d3.z > 0.9f, "and the good map works again");
    }

    follow.clear();
    anim::PoseFollowSystem::uninstall(sys);
    rigs.uninstall(sys);
    std::filesystem::remove_all(g_dir, ec);
    AVER_INFO("{} checks, {} failures", g_checks, g_failures);
    return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
}
