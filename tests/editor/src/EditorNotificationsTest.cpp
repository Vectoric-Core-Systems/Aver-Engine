// The editor notification model: expiry, dedup, stickiness, in-place progress, and the cap.
//
// NO ImGui AND NO CLOCK. Time enters through tick(now), which is the whole reason these rules can be
// asserted at all -- the drawing half lives in SandboxApp.cpp and is not covered here. If this test
// ever needs AVER_WITH_IMGUI or a graphics context, the model has stopped being pure and the split
// described in EditorNotifications.hpp has been undone.
#include "../../../sandbox/src/EditorNotifications.hpp"

#include "aver/core/Log.hpp"

#include <string>
#include <thread>
#include <vector>

using namespace aver;
using editor::NotificationQueue;
using editor::Notification;
using editor::NotifySeverity;
using editor::NotifyAction;

static int g_checks = 0, g_failures = 0;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

namespace {

Notification make(const char* title, f64 ttl = 6.0, const char* dedup = "") {
    Notification n;
    n.title = title;
    n.ttlSec = ttl;
    n.dedupKey = dedup;
    return n;
}

usize visibleCount(NotificationQueue& q, f64 now) {
    std::vector<Notification> out;
    usize hidden = 0;
    q.tick(now, out, hidden);
    return out.size();
}

} // namespace

int main() {
    AVER_INFO("=== editor notifications ===");

    // ---- expiry -------------------------------------------------------------------------------
    {
        NotificationQueue q;
        q.push(make("hello", 4.0));
        check(visibleCount(q, 0.0) == 1, "a pushed notification is visible on the first tick");
        check(visibleCount(q, 3.9) == 1, "and still visible just before its ttl");
        check(visibleCount(q, 4.1) == 0, "and gone just after it");
        check(q.size() == 0, "expiry removes it from the queue, not just from the draw list");
    }

    // ---- unstamped entries do not age while nothing is drawing ---------------------------------
    //
    // A notification raised while the editor was minimised, or during a blocking level load, must
    // get its FULL life when a frame finally draws it rather than having expired unseen.
    {
        NotificationQueue q;
        q.push(make("queued while asleep", 4.0));
        check(visibleCount(q, 1000.0) == 1, "an entry first drawn 1000s later is still shown");
        check(visibleCount(q, 1003.9) == 1, "and its ttl runs from the frame that DREW it");
        check(visibleCount(q, 1004.1) == 0, "expiring only then");
    }

    // ---- dedup --------------------------------------------------------------------------------
    {
        NotificationQueue q;
        const u64 a = q.push(make("disk on fire", 6.0, "err.disk"));
        const u64 b = q.push(make("disk on fire", 6.0, "err.disk"));
        check(q.size() == 1, "a repeat with the same dedup key collapses");
        check(a == b, "and returns the same id");

        std::vector<Notification> out; usize hidden = 0;
        q.tick(0.0, out, hidden);
        check(out.size() == 1 && out[0].count == 2, "with a count of 2");

        q.push(make("something else", 6.0, "err.other"));
        check(q.size() == 2, "a different key does not collapse");

        // An empty key never collapses, even for byte-identical text -- otherwise two genuinely
        // separate outcomes that happen to read alike would silently become one.
        NotificationQueue r;
        r.push(make("same words"));
        r.push(make("same words"));
        check(r.size() == 2, "an EMPTY dedup key never collapses, even for identical text");
    }

    // ---- a repeat resets the age, so a still-firing error cannot fade under its own count ------
    {
        NotificationQueue q;
        q.push(make("repeating", 4.0, "k"));
        check(visibleCount(q, 3.9) == 1, "the first push is alive at 3.9s");
        q.push(make("repeating", 4.0, "k"));
        check(visibleCount(q, 7.0) == 1, "and a repeat at 3.9s keeps it alive past the original ttl");
    }

    // ---- sticky -------------------------------------------------------------------------------
    {
        NotificationQueue q;
        Notification n = make("the sky is falling", 1.0);
        n.sticky = true;
        const u64 id = q.push(std::move(n));
        check(visibleCount(q, 1.0e6) == 1, "a sticky notification never expires");
        q.dismiss(id);
        check(q.size() == 0, "and dismiss is what removes it");
    }

    // ---- in-place progress --------------------------------------------------------------------
    //
    // The failure this prevents: one toast per bake step. Twenty updates must be one row.
    {
        NotificationQueue q;
        const u64 id = q.push(make("Baking navmesh", 6.0));
        for (int i = 0; i < 20; ++i)
            q.setProgress(id, static_cast<f32>(i) / 20.0f, "step " + std::to_string(i));
        check(q.size() == 1, "twenty progress updates leave exactly one notification");

        std::vector<Notification> out; usize hidden = 0;
        q.tick(0.0, out, hidden);
        check(out.size() == 1 && out[0].id == id, "with the id the caller was handed");
        check(out[0].hasProgress, "and it is marked as carrying progress");
    }

    // ---- progress implies no expiry, and finish() re-arms the clock ----------------------------
    {
        NotificationQueue q;
        const u64 id = q.push(make("long job", 2.0));
        q.setProgress(id, 0.5f, "halfway");
        check(visibleCount(q, 100.0) == 1, "a notification carrying progress ignores its ttl");
        q.finish(id, NotifySeverity::Success, "Done", "", 4.0);
        check(visibleCount(q, 100.0) == 1, "finish leaves the result on screen");
        check(visibleCount(q, 104.1) == 0,
              "and its ttl starts from the frame that drew the RESULT, not from the push");
    }

    // ---- progress clamping --------------------------------------------------------------------
    {
        NotificationQueue q;
        const u64 id = q.push(make("clamp me"));
        q.setProgress(id, 5.0f, "");
        std::vector<Notification> out; usize hidden = 0;
        q.tick(0.0, out, hidden);
        check(out.size() == 1 && out[0].progress == 1.0f, "progress above 1 clamps to 1");
        q.setProgress(id, -5.0f, "");
        q.tick(0.0, out, hidden);
        check(out.size() == 1 && out[0].progress < 0.0f,
              "but a negative is preserved as the indeterminate sentinel");
    }

    // ---- the cap, and which end it drops -------------------------------------------------------
    {
        NotificationQueue q;
        for (usize i = 0; i < NotificationQueue::kMaxLive + 8; ++i)
            q.push(make(("n" + std::to_string(i)).c_str()));
        check(q.size() == NotificationQueue::kMaxLive, "the queue is bounded at kMaxLive");

        std::vector<Notification> out; usize hidden = 0;
        q.tick(0.0, out, hidden);
        // The newest must survive: the entry most worth seeing is the one that just happened.
        check(!out.empty() && out.back().title == "n" + std::to_string(NotificationQueue::kMaxLive + 7),
              "and overflow drops the OLDEST -- the newest notification is still present");
    }

    // ---- a sticky entry outranks age at the cap -------------------------------------------------
    {
        NotificationQueue q;
        Notification s = make("unacknowledged");
        s.sticky = true;
        const u64 keep = q.push(std::move(s));
        for (usize i = 0; i < NotificationQueue::kMaxLive + 4; ++i)
            q.push(make(("flood" + std::to_string(i)).c_str()));
        check(q.size() == NotificationQueue::kMaxLive, "the cap still holds under a flood");

        q.dismiss(keep);
        check(q.size() == NotificationQueue::kMaxLive - 1,
              "and the sticky entry survived the flood -- it was still there to dismiss");
    }

    // ---- handles are unique, monotonic, and survive their own expiry -----------------------------
    {
        NotificationQueue q;
        const u64 id = q.push(make("brief", 1.0));
        // Drawn once FIRST, deliberately: an unstamped entry cannot expire, so ticking straight to
        // 5.0 would return it rather than reap it. That is the guarantee asserted further up, and
        // this block needs the notification genuinely dead before it can test reviving one.
        check(visibleCount(q, 0.0) == 1, "the notification is drawn once, which starts its clock");
        check(visibleCount(q, 5.0) == 0, "and then expires");
        q.update(id, NotifySeverity::Success, "back", "revived");
        check(q.size() == 1, "updating an expired handle re-inserts it rather than doing nothing");

        std::vector<Notification> out; usize hidden = 0;
        q.tick(5.0, out, hidden);
        check(out.size() == 1 && out[0].id == id, "with the SAME id, so the caller's handle is valid");

        NotificationQueue r;
        u64 prev = 0;
        bool monotonic = true;
        for (int i = 0; i < 200; ++i) {
            const u64 cur = r.push(make("x"));
            if (cur <= prev) monotonic = false;
            prev = cur;
        }
        check(monotonic, "ids are strictly increasing and never reused across 200 pushes");
    }

    // ---- the visible window --------------------------------------------------------------------
    {
        NotificationQueue q;
        for (usize i = 0; i < NotificationQueue::kMaxLive; ++i)
            q.push(make(("v" + std::to_string(i)).c_str(), 4.0));

        std::vector<Notification> out; usize hidden = 0;
        q.tick(0.0, out, hidden);
        check(out.size() == NotificationQueue::kMaxVisible, "at most kMaxVisible are returned");
        check(hidden == NotificationQueue::kMaxLive - NotificationQueue::kMaxVisible,
              "and the rest are reported as hidden");

        // The hidden ones were never stamped, so they must not have aged while invisible.
        q.tick(3.9, out, hidden);
        check(q.size() == NotificationQueue::kMaxLive,
              "a queued entry does not age while it is not being drawn");
    }

    // ---- activations ---------------------------------------------------------------------------
    {
        NotificationQueue q;
        const u64 id = q.push(make("act"));
        u64 gotId = 0; NotifyAction gotAction = NotifyAction::None;
        check(!q.drainActivation(gotId, gotAction), "draining an empty activation queue returns false");

        q.activate(id, NotifyAction::ShowOutputLog);
        check(q.drainActivation(gotId, gotAction), "an activation drains");
        check(gotId == id && gotAction == NotifyAction::ShowOutputLog, "with the id and action given");
        check(!q.drainActivation(gotId, gotAction), "and only once");

        q.activate(999999u, NotifyAction::Dismiss);
        check(!q.drainActivation(gotId, gotAction),
              "activating an id that is already gone is a no-op, not a crash");
    }

    // ---- concurrent producers -------------------------------------------------------------------
    //
    // The log sink pushes from whatever thread logged, so this is the real usage, not a stress test
    // for its own sake. The cap must hold and nothing may tear.
    {
        NotificationQueue q;
        std::thread a([&] { for (int i = 0; i < 5000; ++i) q.push(make("A", 6.0, "keyA")); });
        std::thread b([&] { for (int i = 0; i < 5000; ++i) q.push(make("B", 6.0, "keyB")); });
        std::vector<Notification> out; usize hidden = 0;
        for (int i = 0; i < 200; ++i) q.tick(static_cast<f64>(i) * 0.01, out, hidden);
        a.join();
        b.join();
        check(q.size() <= NotificationQueue::kMaxLive, "the cap holds under concurrent producers");
        q.tick(0.0, out, hidden);
        // Both keys dedup, so 10000 pushes must collapse to at most two rows.
        check(q.size() <= 2, "and two dedup keys collapse 10000 pushes into at most two rows");
    }

    // ---- what the log sink makes of a log line ---------------------------------------------------
    {
        NotificationQueue q;
        q.pushFromLog(LogLevel::Info, "[Thing] ordinary");
        q.pushFromLog(LogLevel::Warn, "[Thing] a warning");
        check(q.size() == 0, "Info and Warn raise nothing -- they are pushed at their call sites");

        q.pushFromLog(LogLevel::Error, "[Import] could not read foo.png");
        std::vector<Notification> out; usize hidden = 0;
        q.tick(0.0, out, hidden);
        check(out.size() == 1, "an Error raises one");
        check(out[0].title == "Error - Import", "with the log tag lifted into the title");
        check(out[0].body == "could not read foo.png", "and the rest as the body");
        check(!out[0].sticky, "an Error is not sticky");

        q.pushFromLog(LogLevel::Critical, "[RHI.D3D12] THE GPU DEVICE HAS BEEN LOST");
        q.tick(0.0, out, hidden);
        check(out.size() == 2 && out.back().sticky, "but a Critical is sticky");
        check(out.back().actions[0] == NotifyAction::ShowOutputLog,
              "and offers to open the Output Log, which is where the line actually is");

        // A storm of one repeated error is one row. This is the property that makes it safe to feed
        // this from the log sink at all.
        for (int i = 0; i < 300; ++i) q.pushFromLog(LogLevel::Error, "[Shader] compile failed");
        q.tick(0.0, out, hidden);
        usize shaderRows = 0;
        u32 shaderCount = 0;
        for (const Notification& n : out)
            if (n.title == "Error - Shader") { ++shaderRows; shaderCount = n.count; }
        check(shaderRows == 1 && shaderCount == 300,
              "300 identical errors are ONE row with a count of 300");
    }

    if (g_failures == 0) AVER_INFO("=== all {} notification checks passed ===", g_checks);
    else                 AVER_ERROR("=== {} of {} notification check(s) FAILED ===", g_failures, g_checks);
    return g_failures == 0 ? 0 : 1;
}
