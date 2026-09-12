// NOTIFY STATES: a window that opens, stays open and closes, and the four ways an open one leaks.
//
// This is deliberately a SEPARATE executable from AnimSystemTest rather than more cases appended to
// its "a playing clip fires its notifies" section: that section proves an INSTANT notify's interval
// arithmetic, and this proves something that has no meaning for one at all -- that a second event
// (the close) is GUARANTEED to follow the first (the open), no matter what happens to the clip or the
// entity in between. A test that only ever plays a clip forward to its own natural end could not
// tell "closes correctly" from "never had a bug", because normal playback is the one shape that was
// never the risk -- see AnimSystem.hpp's own banner on stepNotifyStates for the other three.
//
// Each of the four sections below leaves an entity with a notify state OPEN and then does the one
// thing that is supposed to close it anyway, checking the "_End" fires from THAT and not from a
// later, ordinary crossing -- which is the only way to tell "this path closes it" from "some other
// path eventually would have".
#include "aver/anim/AnimSystem.hpp"
#include "aver/core/Log.hpp"
#include "aver/formats/OcAnim.hpp"

#include <filesystem>
#include <string>
#include <vector>

using namespace aver;

static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

static std::string g_dir;
// Windup [0.3, 0.8]: a normal, fully-in-clip window -- leak (a), and the clip switch/destroy cases
// reuse it too, since "closes some other way before its natural end" is exactly what those two are.
// Mid, at 1.0, is a PLAIN instant notify sitting among the states, so a regression that started
// treating every notify as a state (or vice versa) would show up here.
// NearEnd [1.6, 2.3-clamped-to-2.0]: a window whose raw end overruns the clip, so it can ONLY ever
// close at the loop seam -- leak (d)'s exemplar.
static u64 kClipAId = 0xA5171E0001ull;
// A second, unrelated clip with no notifies at all, for leak (b): switching an animator onto it
// mid-window must close whatever was open under clip A first.
static u64 kClipBId = 0xA5171E0002ull;

static std::string resolvePath(u64 id, void*) {
    if (id == kClipAId) return g_dir + "/a.ocanim";
    if (id == kClipBId) return g_dir + "/b.ocanim";
    return {};
}

static std::vector<std::string> g_fired;
static void recordNotify(scene::Entity, const char* name, void*) { g_fired.emplace_back(name); }
static std::string firedSeq() {
    std::string out;
    for (const std::string& n : g_fired) { if (!out.empty()) out += " "; out += n; }
    return out;
}

int main() {
    AVER_INFO("AnimNotifyStateTest");

    g_dir = (std::filesystem::temp_directory_path() / "aver-animnotifystate-test").string();
    std::error_code ec;
    std::filesystem::create_directories(g_dir, ec);

    {
        // One bone, one track -- the pose is not what this test is about, but writeOcAnim refuses a
        // clip with no tracks at all.
        fmt::OcTrack t;
        t.boneIndex = 0;
        t.channels = fmt::kOcChannelTranslation;
        t.times = {0.0f, 2.0f};
        t.values = {0, 0, 0,  0, 0, 0};

        fmt::OcAnimation a;
        a.duration = 2.0f;
        a.tracks.push_back(t);
        a.notifies = {{0.3f, "Windup"}, {1.0f, "Mid"}, {1.6f, "NearEnd"}};
        a.notifyDurations = {0.5f, 0.0f, 0.7f};   // Windup and NearEnd are states; Mid stays instant
        std::string why;
        check(fmt::saveOcAnim(g_dir + "/a.ocanim", a, &why), "clip A writes: " + why);

        fmt::OcAnimation b;
        b.duration = 1.0f;
        fmt::OcTrack t2 = t;
        t2.times = {0.0f, 1.0f};
        b.tracks.push_back(t2);
        // No notifies at all -- switching onto this clip must close anything clip A left open.
        check(fmt::saveOcAnim(g_dir + "/b.ocanim", b, &why), "clip B writes: " + why);
    }

    scene::World& w = scene::World::instance();
    anim::AnimSystem& sys = anim::animSystem();
    sys.clear();
    sys.setResolver(&resolvePath, nullptr);
    sys.setNotifySink(&recordNotify, nullptr);

    AVER_INFO("(a) normal playback closes a state out its own far side, and reopens next lap");
    {
        const scene::Entity e = w.create("leakA");
        auto* a = static_cast<scene::CAnimator*>(w.addComponent(e, scene::kComponentAnimator));
        a->clip = kClipAId;

        g_fired.clear();
        sys.tick(w, 0.2f);                 // 0 -> 0.2, catch-up first tick: nothing sits at/before 0
        check(firedSeq().empty(), "nothing at the very start of clip A");

        g_fired.clear();
        sys.tick(w, 0.2f);                 // 0.2 -> 0.4, straddles Windup's start at 0.3
        check(firedSeq() == "Windup_Begin", "crossing 0.3 opens the state, got '" + firedSeq() + "'");

        g_fired.clear();
        sys.tick(w, 0.3f);                 // 0.4 -> 0.7, still short of 0.8
        check(firedSeq().empty(), "and stays open with nothing fired mid-window");

        g_fired.clear();
        sys.tick(w, 0.2f);                 // 0.7 -> 0.9, straddles Windup's end at 0.8
        check(firedSeq() == "Windup_End", "crossing 0.8 closes it, got '" + firedSeq() + "'");

        g_fired.clear();
        sys.tick(w, 0.3f);                 // 0.9 -> 1.2, straddles Mid's instant at 1.0
        check(firedSeq() == "Mid", "a PLAIN notify among states still fires unsuffixed, got '" +
                                   firedSeq() + "'");

        // ---- (d) THE LOOP SEAM: NearEnd's raw window is [1.6, 2.3], past the clip's own 2.0s.
        g_fired.clear();
        sys.tick(w, 0.3f);                 // 1.2 -> 1.5, short of 1.6
        check(firedSeq().empty(), "not yet at NearEnd");

        g_fired.clear();
        sys.tick(w, 0.2f);                 // 1.5 -> 1.7, straddles NearEnd's start at 1.6
        check(firedSeq() == "NearEnd_Begin", "NearEnd opens, got '" + firedSeq() + "'");

        // A big enough step to overrun the clip and wrap: 1.7 -> 2.2 -> wraps to 0.2. NearEnd's
        // window would not naturally end until 2.3, which this clip does not have -- the ONLY way it
        // can ever close is the loop seam forcing it at 2.0.
        g_fired.clear();
        sys.tick(w, 0.5f);
        check(firedSeq() == "NearEnd_End",
              "the wrap force-closes it exactly at the seam, got '" + firedSeq() + "'");

        // ---- the open flags are per-notify and really did reset, not just coincidentally quiet
        g_fired.clear();
        sys.tick(w, 0.2f);                 // 0.2 -> 0.4 on the NEW lap, straddles 0.3 again
        check(firedSeq() == "Windup_Begin",
              "Windup opens again on the next lap -- its flag was not left stuck open, got '" +
              firedSeq() + "'");
        // Leave it open on purpose; the destroy/switch sections below start their own entities.

        w.destroy(e);
        w.flush();
        sys.tick(w, 0.05f);   // the prune runs here; see the destroy section for what it must do
        g_fired.clear();
    }

    AVER_INFO("(b) the clip changing while a window is open closes it, even mid-range");
    {
        const scene::Entity e = w.create("leakB");
        auto* a = static_cast<scene::CAnimator*>(w.addComponent(e, scene::kComponentAnimator));
        a->clip = kClipAId;

        g_fired.clear();
        sys.tick(w, 0.2f);   // 0 -> 0.2
        sys.tick(w, 0.2f);   // 0.2 -> 0.4, opens Windup
        check(firedSeq() == "Windup_Begin", "Windup is open, got '" + firedSeq() + "'");

        // STILL WELL SHORT OF 0.8 -- an ordinary crossing would not close this for another four
        // ticks at this rate. Switching clips is the only thing that should close it here.
        g_fired.clear();
        a->clip = kClipBId;
        sys.tick(w, 0.1f);
        check(firedSeq() == "Windup_End",
              "pointing the animator at a different clip force-closes it, got '" + firedSeq() + "'");

        // And it does not come back: clip B has no notifies at all.
        g_fired.clear();
        for (int i = 0; i < 5; ++i) sys.tick(w, 0.1f);
        check(firedSeq().empty(), "clip B, having no notifies, fires nothing at all");

        w.destroy(e);
        w.flush();
        sys.tick(w, 0.05f);
        g_fired.clear();
    }

    AVER_INFO("(c) the entity being destroyed mid-window closes it");
    {
        const scene::Entity e = w.create("leakC");
        auto* a = static_cast<scene::CAnimator*>(w.addComponent(e, scene::kComponentAnimator));
        a->clip = kClipAId;

        g_fired.clear();
        sys.tick(w, 0.2f);   // 0 -> 0.2
        sys.tick(w, 0.2f);   // 0.2 -> 0.4, opens Windup
        check(firedSeq() == "Windup_Begin", "Windup is open, got '" + firedSeq() + "'");

        // Gone, with the window still numerically inside [0.3, 0.8] -- there is no later tick on
        // this entity that could ever reach 0.8 to close it the ordinary way.
        w.destroy(e);
        w.flush();

        g_fired.clear();
        sys.tick(w, 0.1f);   // the clock-pruning loop at the top of tick() runs here
        check(firedSeq() == "Windup_End",
              "destroying the entity force-closes it, got '" + firedSeq() + "'");

        // And the bookkeeping is really gone, not merely quiet: ticking again fires nothing more,
        // rather than, say, an End repeated every frame forever.
        g_fired.clear();
        sys.tick(w, 0.1f);
        check(firedSeq().empty(), "nothing further fires for an entity that no longer exists");
    }

    AVER_INFO("no sink installed still tracks (no crash, no delivery), matching instant notifies");
    {
        sys.setNotifySink(nullptr, nullptr);
        check(!sys.hasNotifySink(), "the sink uninstalls");

        const scene::Entity e = w.create("noSink");
        auto* a = static_cast<scene::CAnimator*>(w.addComponent(e, scene::kComponentAnimator));
        a->clip = kClipAId;

        for (int i = 0; i < 6; ++i) sys.tick(w, 0.2f);   // would have opened and closed Windup
        w.destroy(e);
        w.flush();
        sys.tick(w, 0.1f);   // and this would have force-closed it on destroy
        // The only assertion possible with no sink is that none of the above crashed or hung --
        // exactly AnimSystemTest's own "no sink means no delivery" case, applied to a state.
        check(true, "tracking with no sink installed neither crashes nor leaves anything to observe");

        sys.setNotifySink(&recordNotify, nullptr);
    }

    std::filesystem::remove_all(g_dir, ec);
    AVER_INFO(g_failures ? "AnimNotifyStateTest: {} FAILURES" : "AnimNotifyStateTest: all checks passed ({})",
              g_failures);
    return g_failures ? 1 : 0;
}
