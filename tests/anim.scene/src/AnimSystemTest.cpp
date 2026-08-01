// The scene/animation join: components, the clock, and the pose it produces.
//
// The failure this exists to catch is the one this codebase keeps having -- a component that
// registers, marshals and stores perfectly and that NOTHING EVER READS. CLight and CCamera are both
// live proof. So the assertions here are about the tick actually moving something, not about the
// components existing.
#include "aver/anim/AnimSystem.hpp"
#include "aver/core/Log.hpp"
#include "aver/formats/OcAnim.hpp"

#include <cmath>
#include <filesystem>
#include <string>

using namespace aver;

static int g_failures = 0;

// Records one assertion.
static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

// Records one assertion that two floats agree to `eps`.
static void checkNear(f32 got, f32 want, f32 eps, const std::string& what) {
    if (std::fabs(got - want) <= eps) { AVER_INFO("  ok    {} ({:.4f})", what, got); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {} (got {:.5f}, want {:.5f})", what, got, want);
}

// Where the fixture assets are written, and what the resolver maps ids onto.
static std::string g_dir;
static u64 kSkelId = 0x5EE10001ull, kClipId = 0xC11B0001ull;

// The host's job: an id to a path. Deliberately a plain function, because the system takes a
// function pointer rather than owning any idea of where content lives.
static std::string resolvePath(u64 id, void*) {
    if (id == kSkelId) return g_dir + "/rig.ocskel";
    if (id == kClipId) return g_dir + "/clip.ocanim";
    return {};
}

int main() {
    AVER_INFO("AnimSystemTest");

    g_dir = (std::filesystem::temp_directory_path() / "aver-animsystem-test").string();
    std::error_code ec;
    std::filesystem::create_directories(g_dir, ec);

    // A two-bone rig and a clip that slides the child 100 cm along X over one second.
    {
        fmt::OcSkeleton s;
        fmt::OcBone root;  root.name = "root";
        fmt::OcBone child; child.name = "child"; child.parent = 0; child.translation = Vec3{0, 0, 50};
        s.bones = {root, child};
        std::string why;
        check(fmt::saveOcSkel(g_dir + "/rig.ocskel", s, &why), "the fixture rig writes: " + why);

        fmt::OcAnimation a;
        a.duration = 1.0f;
        a.flags = fmt::kOcAnimLoop;
        fmt::OcTrack t;
        t.boneIndex = 1;
        t.channels = fmt::kOcChannelTranslation;
        t.interp = fmt::OcInterp::Linear;
        t.times = {0.0f, 1.0f};
        t.values = {0, 0, 50,  100, 0, 50};
        a.tracks.push_back(t);
        check(fmt::saveOcAnim(g_dir + "/clip.ocanim", a, &why), "the fixture clip writes: " + why);
    }

    scene::World& w = scene::World::instance();
    anim::AnimSystem& sys = anim::animSystem();
    sys.clear();
    sys.setResolver(&resolvePath, nullptr);

    AVER_INFO("the components register");
    {
        check(w.componentId("CSkeletalMesh") == scene::kComponentSkeletalMesh,
              "CSkeletalMesh landed on the id the ABI publishes");
        check(w.componentId("CAnimator") == scene::kComponentAnimator,
              "and CAnimator on its own");
        check(w.componentVerified(scene::kComponentSkeletalMesh) &&
              w.componentVerified(scene::kComponentAnimator),
              "both field tables cover the struct with no gap -- a padded one aborts World's ctor");
        check(w.fieldId("CAnimator.time") != 0 && w.fieldId("CSkeletalMesh.skeleton") != 0,
              "and their fields resolve by qualified name, which is how C# reaches them");
    }

    AVER_INFO("the clock advances, and it does NOT need an asset to");
    {
        const scene::Entity e = w.create("clockOnly");
        auto* a = static_cast<scene::CAnimator*>(w.addComponent(e, scene::kComponentAnimator));
        check(a != nullptr, "an animator attaches");
        checkNear(a->time, 0.0f, 1e-6f, "starting at zero");
        // A zero-filled component must be a working one: addComponent does not run member
        // initialisers, so anything that reads 0 as "off" attaches and silently does nothing.
        check((a->flags & scene::kAnimatorPaused) == 0, "and playing by default, from ZERO-FILLED bytes");
        checkNear(a->speed, 0.0f, 1e-6f, "even though speed reads back as a literal 0");

        sys.tick(w, 0.25f);
        checkNear(a->time, 0.25f, 1e-5f, "one tick advances it by dt");
        a->speed = 2.0f;
        sys.tick(w, 0.25f);
        checkNear(a->time, 0.75f, 1e-5f, "and an explicit speed scales that");
        a->flags |= scene::kAnimatorPaused;
        sys.tick(w, 0.25f);
        checkNear(a->time, 0.75f, 1e-5f, "a stopped animator does not move");
        w.destroy(e);
        w.flush();
    }

    AVER_INFO("a rig and a clip produce a POSE");
    {
        const scene::Entity e = w.create("posed");
        auto* sm = static_cast<scene::CSkeletalMesh*>(w.addComponent(e, scene::kComponentSkeletalMesh));
        auto* a  = static_cast<scene::CAnimator*>(w.addComponent(e, scene::kComponentAnimator));
        sm->skeleton = kSkelId;
        a->clip = kClipId;

        sys.tick(w, 0.5f);
        check(sys.loadedSkeletons() == 1 && sys.loadedClips() == 1, "both assets loaded once");
        check(sm->boneCount == 2, "the resolved bone count is published back onto the component");
        check(sm->dirty == 0, "and the dirty flag cleared");

        const anim::Pose* p = sys.pose(e);
        check(p != nullptr && p->local.size() == 2, "a pose exists, one transform per bone");
        if (p) checkNear(p->local[1].position.x, 50.0f, 1e-3f,
                         "half a second into a 0..100 slide is 50 -- the clip is being SAMPLED");

        u32 n = 0;
        const Mat4* skin = sys.skinning(e, n);
        check(skin != nullptr && n == 2, "and the skinning matrices are there");
        // The child's model position is its own 50 up plus the clip's 50 along X.
        if (skin) checkNear(skin[1].m[3][0], 50.0f, 1e-2f, "which carry the sampled translation");

        // Looping is the COMPONENT's business, not the clip's, so one clip can loop for one actor
        // and play once for another.
        a->time = 1.25f;
        sys.tick(w, 0.0f);
        p = sys.pose(e);
        if (p) checkNear(p->local[1].position.x, 25.0f, 1e-3f, "a looping animator wraps past the end");

        a->flags |= scene::kAnimatorOnce;
        a->time = 1.25f;
        sys.tick(w, 0.0f);
        p = sys.pose(e);
        if (p) checkNear(p->local[1].position.x, 100.0f, 1e-3f, "and a one-shot clamps at it instead");

        w.destroy(e);
        w.flush();
    }

    AVER_INFO("a missing asset degrades rather than spinning");
    {
        sys.clear();
        sys.setResolver(&resolvePath, nullptr);
        const scene::Entity e = w.create("broken");
        auto* sm = static_cast<scene::CSkeletalMesh*>(w.addComponent(e, scene::kComponentSkeletalMesh));
        auto* a  = static_cast<scene::CAnimator*>(w.addComponent(e, scene::kComponentAnimator));
        sm->skeleton = 0xDEADBEEFull;   // the resolver returns "" for this
        a->clip = kClipId;

        for (int i = 0; i < 5; ++i) sys.tick(w, 0.1f);
        checkNear(a->time, 0.5f, 1e-5f, "the clock still runs with no rig");
        check(sys.pose(e) == nullptr, "and no pose is invented");
        check(sys.loadedSkeletons() == 1,
              "the failure is cached ONCE -- otherwise a missing file is reopened every frame forever");
        w.destroy(e);
        w.flush();
    }

    std::filesystem::remove_all(g_dir, ec);
    AVER_INFO(g_failures ? "AnimSystemTest: {} FAILURES" : "AnimSystemTest: all checks passed ({})",
              g_failures);
    return g_failures ? 1 : 0;
}
