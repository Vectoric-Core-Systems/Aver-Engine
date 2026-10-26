// Aver.World: the floating-origin service protocol (LARGE_WORLD 2.2.3). Exit code = failure count.
//
// Participants here are fakes that record what they were asked, in which order, and what origin they could
// see; the real participants arrive with the hosts' wiring. Every property is a statement about the
// two-phase protocol: a refusal changes NOTHING, a commit sees the new world.
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"
#include "aver/world/WorldOrigin.hpp"

#include <chrono>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace aver;
using namespace aver::world;

static int g_checks = 0;
static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) return;
    AVER_ERROR("   FAIL  {}", what);
    ++g_failures;
}

static int g_warns = 0;
static void countWarns(void*, LogLevel level, std::string_view) { if (level == LogLevel::Warn) ++g_warns; }

int main() {
    const i64 STEP = kOriginStepCm;

    // ---- order: by `order`, then registration; each once per shift ---------------------------------------
    {
        WorldOrigin w;
        std::vector<std::string> prep, comm;
        auto add = [&](const char* name, i32 order) {
            Participant p;
            p.name = name; p.order = order;
            p.prepare = [&prep, name](const ShiftEvent&) { prep.push_back(name); return PrepareResult{}; };
            p.commit  = [&comm, name](const ShiftEvent&) { comm.push_back(name); };
            w.addParticipant(p);
        };
        add("scene", 20); add("physics-b", 10); add("render", 60); add("physics-a", 10); add("host", 30);
        check(w.participantCount() == 5, "five participants registered");
        check(w.shiftBy(OriginCm{STEP, 0}), "shift commits");
        const std::vector<std::string> want = {"physics-b", "physics-a", "scene", "host", "render"};
        check(prep == want, "prepare runs in order then registration order, once each");
        check(comm == want, "commit runs in order then registration order, once each");
        check(w.origin().x == STEP && w.generation() == 1 && w.stats().shifts == 1, "origin, generation, stats after one shift");
    }

    // ---- a prepare refusal changes nothing ------------------------------------------------------------------
    {
        WorldOrigin w;
        int commits = 0, prepares = 0;
        bool refuse = true;
        Participant a;  a.name = "a"; a.order = 10;
        a.prepare = [&](const ShiftEvent&) { ++prepares; return PrepareResult{}; };
        a.commit  = [&](const ShiftEvent&) { ++commits; };
        Participant b;  b.name = "gate"; b.order = 20;
        b.prepare = [&](const ShiftEvent&) {
            ++prepares;
            return refuse ? PrepareResult{false, "gesture active"} : PrepareResult{};
        };
        b.commit  = [&](const ShiftEvent&) { ++commits; };
        Participant c;  c.name = "c"; c.order = 30;
        c.prepare = [&](const ShiftEvent&) { ++prepares; return PrepareResult{}; };
        c.commit  = [&](const ShiftEvent&) { ++commits; };
        w.addParticipant(a); w.addParticipant(b); w.addParticipant(c);

        setLogSink(&countWarns, nullptr);
        g_warns = 0;
        check(!w.shiftBy(OriginCm{0, STEP}), "a refusing prepare makes shiftBy return false");
        check(commits == 0, "no commit ran");
        check(prepares == 2, "prepare stopped at the first refusal (third never asked)");
        check(w.origin() == OriginCm{} && w.generation() == 0, "origin and generation unchanged");
        check(w.stats().deferrals == 1 && w.stats().shifts == 0, "deferrals +1, no shift counted");
        check(w.stats().lastDeferReason && std::string(w.stats().lastDeferReason) == "gesture active", "reason recorded");

        // at most one log line per second for a refusal storm
        for (int i = 0; i < 19; ++i) w.shiftBy(OriginCm{0, STEP});
        setLogSink(nullptr, nullptr);
        check(g_warns == 1, "20 refusals in a burst log once (" + std::to_string(g_warns) + ")");
        check(w.stats().deferrals == 20, "but are all counted");

        // retried next frame once the gate opens
        refuse = false;
        check(w.update(Vec3{0, 1'000'000.0f, 0}), "the retry commits");
        check(commits == 3 && w.origin().y == STEP && w.generation() == 1, "all three committed, origin moved");
    }

    // ---- commit sees the new origin, prepare the old --------------------------------------------------------
    {
        WorldOrigin w;
        bool prepOld = false, commitNew = false, genOk = false, deltaOk = false;
        Participant p; p.name = "probe";
        p.prepare = [&](const ShiftEvent& ev) {
            prepOld = w.origin() == ev.oldOrigin && w.generation() + 1 == ev.generation;
            return PrepareResult{};
        };
        p.commit = [&](const ShiftEvent& ev) {
            commitNew = w.origin() == ev.newOrigin;
            genOk = w.generation() == ev.generation;
            deltaOk = ev.deltaCm.x == static_cast<f64>(2 * STEP) && ev.deltaCm.y == static_cast<f64>(-STEP) && ev.deltaCm.z == 0.0
                   && ev.deltaF.x == static_cast<f32>(2 * STEP) && ev.deltaF.y == static_cast<f32>(-STEP) && ev.deltaF.z == 0.0f
                   && ev.newOrigin.x - ev.oldOrigin.x == 2 * STEP;
        };
        w.addParticipant(p);
        w.shiftBy(OriginCm{2 * STEP, -STEP});
        check(prepOld, "prepare sees the old origin and generation + 1 in the event");
        check(commitNew && genOk, "commit sees the new origin and generation");
        check(deltaOk, "the event carries the delta in f64 and f32");
        // absolute()/local() follow the live origin
        const WorldPos at = w.absolute(Vec3{100, 200, 300});
        check(absOf(at).x == static_cast<f64>(2 * STEP) + 100.0 && absOf(at).y == static_cast<f64>(-STEP) + 200.0, "absolute() uses the origin");
        const Vec3 back = w.local(at);
        check(back.x == 100.0f && back.y == 200.0f && back.z == 300.0f, "local() inverts absolute()");
    }

    // ---- policies --------------------------------------------------------------------------------------------
    {
        int commits = 0;
        Participant p; p.name = "n"; p.commit = [&](const ShiftEvent&) { ++commits; };

        WorldOrigin fixed;
        WorldOrigin::Settings s; s.policy = OriginPolicy::Fixed;
        fixed.configure(s);
        fixed.addParticipant(p);
        check(!fixed.update(Vec3{5e6f, 0, 0}), "Fixed: update never shifts");
        check(!fixed.shiftBy(OriginCm{STEP, 0}), "Fixed: shiftBy refuses too");
        check(commits == 0 && fixed.origin() == OriginCm{}, "Fixed: nothing ran, origin unchanged");
        fixed.reset(OriginCm{3 * STEP, -2 * STEP});
        check(fixed.origin() == OriginCm{3 * STEP, -2 * STEP}, "Fixed: reset still sets the origin");

        commits = 0;
        WorldOrigin manual;
        s.policy = OriginPolicy::Manual;
        manual.configure(s);
        manual.addParticipant(p);
        check(!manual.update(Vec3{5e6f, 0, 0}), "Manual: update never shifts");
        check(manual.shiftBy(OriginCm{STEP, STEP}), "Manual: shiftBy shifts");
        check(commits == 1 && manual.origin() == OriginCm{STEP, STEP}, "Manual: one commit");

        commits = 0;
        WorldOrigin off;
        s.policy = OriginPolicy::FollowCamera; s.enabled = false;
        off.configure(s);
        off.addParticipant(p);
        check(!off.enabled(), "enabled() reports the setting");
        check(!off.update(Vec3{5e6f, 0, 0}), "enabled=false: update is a no-op");
        check(!off.shiftBy(OriginCm{STEP, 0}), "enabled=false: shiftBy is a no-op");
        check(commits == 0, "enabled=false: nothing ran");
    }

    // ---- reset runs no participant ----------------------------------------------------------------------------
    {
        WorldOrigin w;
        int calls = 0;
        Participant p; p.name = "n";
        p.prepare = [&](const ShiftEvent&) { ++calls; return PrepareResult{}; };
        p.commit  = [&](const ShiftEvent&) { ++calls; };
        w.addParticipant(p);
        w.reset(OriginCm{4 * STEP, 0});
        check(calls == 0, "reset runs no participant");
        check(w.origin().x == 4 * STEP && w.generation() == 1, "reset sets the origin and bumps the generation");
        setLogSink(&countWarns, nullptr);
        g_warns = 0;
        w.reset(OriginCm{STEP + 5, 0});
        setLogSink(nullptr, nullptr);
        check(w.origin().x == STEP && g_warns == 1, "an off-step reset snaps to the nearest region and warns");
    }

    // ---- urgent --------------------------------------------------------------------------------------------------
    {
        WorldOrigin w;
        std::vector<bool> urgent;
        Participant p; p.name = "u";
        p.prepare = [&](const ShiftEvent& ev) { urgent.push_back(ev.urgent); return PrepareResult{}; };
        w.addParticipant(p);
        w.update(Vec3{1'000'000.0f, 0, 0});        // exactly the urgent threshold: not beyond it
        w.update(Vec3{0, -1'000'001.0f, 0});       // beyond on a shifted axis
        w.update(Vec3{950'000.0f, 0, 0});          // below the threshold; shifts non-urgently
        check(urgent.size() == 3, "three shifts reached prepare");
        if (urgent.size() == 3) {
            check(!urgent[0], "1,000,000 cm is not urgent");
            check(urgent[1], "beyond 1,000,000 cm on a shifted axis is urgent");
            check(!urgent[2], "950,000 cm is not urgent");
        }
        // an explicit flag passes through shiftBy
        WorldOrigin m;
        bool seen = false;
        Participant q; q.name = "q"; q.prepare = [&](const ShiftEvent& ev) { seen = ev.urgent; return PrepareResult{}; };
        m.addParticipant(q);
        m.shiftBy(OriginCm{STEP, 0}, true);
        check(seen, "shiftBy(urgent=true) reaches prepare");
    }

    // ---- off-step and zero deltas -----------------------------------------------------------------------------------
    {
        WorldOrigin w;
        int calls = 0;
        Participant p; p.name = "n"; p.prepare = [&](const ShiftEvent&) { ++calls; return PrepareResult{}; };
        w.addParticipant(p);
        setLogSink(&countWarns, nullptr);
        g_warns = 0;
        check(!w.shiftBy(OriginCm{STEP - 1, 0}), "an off-step X delta is refused");
        check(!w.shiftBy(OriginCm{0, 100}), "an off-step Y delta is refused");
        setLogSink(nullptr, nullptr);
        check(!w.shiftBy(OriginCm{0, 0}), "a zero delta is a no-op");
        check(calls == 0 && w.origin() == OriginCm{} && w.generation() == 0 && g_warns == 2, "nothing ran, origin unchanged, refusals logged");
    }

    // ---- re-entrancy: a commit cannot start a shift ------------------------------------------------------------------
    {
        WorldOrigin w;
        bool inner = true;
        Participant p; p.name = "re";
        p.commit = [&](const ShiftEvent&) { inner = w.shiftBy(OriginCm{STEP, 0}); };
        w.addParticipant(p);
        check(w.shiftBy(OriginCm{STEP, 0}) && !inner && w.origin().x == STEP, "a shift started from a commit is refused");
    }

    // ---- per-participant milliseconds ----------------------------------------------------------------------------------
    {
        WorldOrigin w;
        Participant fast; fast.name = "fast"; fast.order = 10; fast.commit = [](const ShiftEvent&) {};
        Participant slow; slow.name = "slow"; slow.order = 20;
        slow.commit = [](const ShiftEvent&) { std::this_thread::sleep_for(std::chrono::milliseconds(8)); };
        w.addParticipant(slow); w.addParticipant(fast);
        w.shiftBy(OriginCm{STEP, 0});
        check(w.stats().lastParticipantMs[1] >= 5.0, "the slow participant's time is recorded by position");
        check(w.stats().lastParticipantMs[0] < w.stats().lastParticipantMs[1], "and the fast one is faster");
        check(w.stats().lastMs >= w.stats().lastParticipantMs[1], "lastMs covers the whole shift");
        check(w.stats().lastParticipantMs[2] == 0.0, "unused slots are zero");
    }

    AVER_INFO("WorldOriginTest: {} checks, {} failures", g_checks, g_failures);
    return g_failures;
}
