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
#include <vector>

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
// A second clip, carrying notifies. Separate from the pose fixture so the existing checks keep
// measuring a clip with none -- which is what proves notifies cost a clip without them nothing.
static u64 kNotifyClipId = 0xC11B0002ull;

// WHAT THE SINK SAW, in order. The host end of the wire in a test is a vector; in the editor it
// is ScriptHost::graphFire. The system cannot tell the difference, which is the point of it
// taking a function pointer.
static std::vector<std::string> g_fired;
static void recordNotify(scene::Entity, const char* name, void*) { g_fired.emplace_back(name); }
// The whole of what the sink saw since the last reset, as one string, so an expectation reads as
// the sequence an author would describe: "B then C".
static std::string firedSeq() {
    std::string out;
    for (const std::string& n : g_fired) { if (!out.empty()) out += " "; out += n; }
    return out;
}

// The host's job: an id to a path. Deliberately a plain function, because the system takes a
// function pointer rather than owning any idea of where content lives.
static std::string resolvePath(u64 id, void*) {
    if (id == kSkelId) return g_dir + "/rig.ocskel";
    if (id == kClipId) return g_dir + "/clip.ocanim";
    if (id == kNotifyClipId) return g_dir + "/notify.ocanim";
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

        // The same clip with markers on it: at the very start, in the middle, and at the very end.
        // Both ends on purpose -- they are where an interval bug shows up as an event that never
        // fires or one that fires twice a loop.
        fmt::OcAnimation n = a;
        n.notifies = {{0.0f, "Start"}, {0.5f, "Mid"}, {1.0f, "End"}};
        check(fmt::saveOcAnim(g_dir + "/notify.ocanim", n, &why), "the notify clip writes: " + why);
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

    AVER_INFO("a recycled entity slot does not inherit the dead entity's pose");
    {
        sys.clear();
        sys.setResolver(&resolvePath, nullptr);

        // Pose an entity, then destroy it. The World reissues its INDEX with a bumped generation,
        // so the next entity created very likely occupies the same slot.
        const scene::Entity dead = w.create("dead");
        {
            auto* sm = static_cast<scene::CSkeletalMesh*>(w.addComponent(dead, scene::kComponentSkeletalMesh));
            auto* a  = static_cast<scene::CAnimator*>(w.addComponent(dead, scene::kComponentAnimator));
            sm->skeleton = kSkelId;
            a->clip = kClipId;
        }
        sys.tick(w, 0.25f);
        u32 n = 0;
        check(sys.skinning(dead, n) != nullptr && n > 0, "the first entity is posed");

        w.destroy(dead);
        w.flush();

        // A FRESH entity, carrying a skeletal mesh but NO animator, so nothing should ever pose it.
        const scene::Entity reborn = w.create("reborn");
        {
            auto* sm = static_cast<scene::CSkeletalMesh*>(w.addComponent(reborn, scene::kComponentSkeletalMesh));
            sm->skeleton = kSkelId;
        }
        check(scene::entityIndex(reborn) == scene::entityIndex(dead),
              "the slot really was recycled, so this case tests what it claims to");
        check(reborn != dead, "but the handle differs, because the generation moved");

        sys.tick(w, 0.25f);
        n = 0;
        check(sys.skinning(reborn, n) == nullptr,
              "an entity with NO animator has NO skinning matrices -- inheriting the dead "
              "entity's would put a new character in a stranger's pose");
        check(sys.pose(reborn) == nullptr, "nor a pose");
        check(sys.skinning(dead, n) == nullptr, "and the dead handle resolves to nothing at all");

        w.destroy(reborn);
        w.flush();
    }

    AVER_INFO("a playing clip fires its notifies");
    {
        sys.setNotifySink(&recordNotify, nullptr);
        check(sys.hasNotifySink(), "the sink installs");

        const scene::Entity e = w.create("notified");
        auto* a = static_cast<scene::CAnimator*>(w.addComponent(e, scene::kComponentAnimator));
        a->clip = kNotifyClipId;

        // NO SKELETAL MESH ON THIS ENTITY AT ALL. A notify is an event on a clock, and the clock
        // runs whether or not a rig ever resolved -- an audio cue on an unrigged prop is not less
        // real for having no bones. If this ever regresses to "only posed entities fire", this is
        // the check that says so.
        check(!w.component<scene::CSkeletalMesh>(e, scene::kComponentSkeletalMesh),
              "the entity has no rig, deliberately");

        g_fired.clear();
        sys.tick(w, 0.1f);
        check(firedSeq() == "Start",
              "the first tick fires the marker AT ZERO -- the one a half-open interval would lose");

        g_fired.clear();
        sys.tick(w, 0.1f);   // 0.1 -> 0.2
        check(firedSeq().empty(), "a tick over empty clip time fires nothing");

        g_fired.clear();
        sys.tick(w, 0.4f);   // 0.2 -> 0.6, over Mid
        check(firedSeq() == "Mid", "and a tick that crosses one fires exactly it");

        g_fired.clear();
        sys.tick(w, 0.2f);   // 0.6 -> 0.8
        check(firedSeq().empty(), "NOT AGAIN on the next tick, which is the double-fire this guards");

        // Round the loop: 0.8 -> 1.1 wraps to 0.1, crossing End (at 1.0) and Start (at 0).
        g_fired.clear();
        sys.tick(w, 0.3f);
        check(firedSeq() == "Start End" || firedSeq() == "End Start",
              "a loop fires the end of the clip and its start, both");

        // ---- a paused animator is being INSPECTED, not played
        g_fired.clear();
        a->flags |= scene::kAnimatorPaused;
        a->time = 0.9f;      // a script (or a scrubbing editor) jumping the playhead
        sys.tick(w, 0.1f);
        check(firedSeq().empty(), "scrubbing a PAUSED animator across a marker fires nothing");
        a->flags &= ~scene::kAnimatorPaused;
        g_fired.clear();
        sys.tick(w, 0.05f);  // 0.9 -> 0.95, still short of End
        check(firedSeq().empty(),
              "and resuming continues from where the scrub left it, not from where it was paused");

        // ---- a step that swallows the clip
        g_fired.clear();
        sys.tick(w, 10.0f);
        check(g_fired.size() == 3, "a stalled frame delivers each marker ONCE, not once per lap");

        // ---- pointing the animator at a different clip forgets the old playhead
        g_fired.clear();
        a->clip = kClipId;   // the notify-free fixture
        sys.tick(w, 0.1f);
        check(firedSeq().empty(), "a clip with no notifies fires none");

        const u64 firedBefore = sys.notifiesFired();
        check(firedBefore > 0, "the system counts what it delivered");

        // ---- no sink means no delivery, and NO BACKLOG when one is installed later
        sys.setNotifySink(nullptr, nullptr);
        check(!sys.hasNotifySink(), "the sink uninstalls");
        a->clip = kNotifyClipId;
        a->time = 0.0f;
        g_fired.clear();
        sys.tick(w, 0.6f);   // would have crossed Start and Mid
        check(firedSeq().empty(), "with no sink installed, nothing is delivered");
        check(sys.notifiesFired() == firedBefore, "and nothing is counted");
        sys.setNotifySink(&recordNotify, nullptr);
        g_fired.clear();
        sys.tick(w, 0.1f);   // 0.6 -> 0.7, crosses nothing
        check(firedSeq().empty(),
              "installing a sink mid-session delivers NO BACKLOG -- the crossings it missed are gone, "
              "not queued");

        w.destroy(e);
        w.flush();
        sys.tick(w, 0.1f);   // the prune runs here
        sys.setNotifySink(nullptr, nullptr);
    }

    std::filesystem::remove_all(g_dir, ec);
    AVER_INFO(g_failures ? "AnimSystemTest: {} FAILURES" : "AnimSystemTest: all checks passed ({})",
              g_failures);
    return g_failures ? 1 : 0;
}
