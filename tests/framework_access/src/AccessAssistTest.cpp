// Input assist: AccessAssist (the toggle latch, the exempt set, the timing scales) and the trigger engine's Timing hook
// (ActionEngine.cpp, compiled into this test). Hold and HoldRelease times shrink, Tap and Combo windows grow, Pulse,
// Chord and Down are untouched, and a null Timing is exactly the old behaviour. Exits 0 or 1.
#include "ActionEngine.hpp"

#include "aver/core/Log.hpp"
#include "aver/formats/OcInput.hpp"
#include "aver/framework/AccessAssist.hpp"

#include <cmath>
#include <map>
#include <string>
#include <vector>

using namespace aver;
using namespace aver::fw::access;
namespace fi = aver::fwinput;

static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    if (cond) return;
    AVER_ERROR("   FAIL  {}", what);
    ++g_failures;
}

// ---- the latch ---------------------------------------------------------------------------------------

static void testToggle() {
    AVER_INFO("=== toggle instead of hold ===");
    AccessAssist a;
    f32 v[3] = {1, 0, 0};
    a.filter("Sprint", true, v);
    check(v[0] == 1.0f, "off by default: the value passes through");

    a.setToggle("Sprint", true);
    const auto frame = [&](f32 x) { f32 w[3] = {x, 0, 0}; a.filter("Sprint", true, w); return w[0]; };
    check(frame(0.0f) == 0.0f, "idle reads 0");
    check(frame(1.0f) == 1.0f, "a press latches it on");
    check(frame(1.0f) == 1.0f, "holding the key does not flip it again");
    check(frame(0.0f) == 1.0f && a.latched("Sprint"), "releasing keeps it on: the latch holds the last value");
    check(frame(0.0f) == 1.0f, "and keeps holding");
    check(frame(1.0f) == 0.0f, "the next press releases it");
    check(frame(0.0f) == 0.0f && !a.latched("Sprint"), "and it stays off");

    // The held value is the last real one.
    frame(0.8f);
    check(frame(0.0f) == 0.8f, "the latch repeats the last held value");

    // Axis actions ignore it.
    f32 ax[3] = {0.0f, 0.0f, 0.0f};
    f32 ay[3] = {0.5f, 0.2f, 0.0f};
    a.filter("Sprint", false, ay);
    check(ay[0] == 0.5f && ay[1] == 0.2f, "an axis action is left alone");
    a.filter("Sprint", false, ax);

    // Exempt actions ignore the saved choice.
    a.setAllowed("Sprint", false);
    check(frame(1.0f) == 1.0f && frame(0.0f) == 0.0f && a.toggleChosen("Sprint") && !a.toggleOn("Sprint"), "an exempt action ignores the toggle but remembers the choice");
    a.setAllowed("Sprint", true);
    check(a.toggleOn("Sprint"), "lifting the exemption restores it");

    // Reset clears the latch but not the choice.
    frame(1.0f);
    check(a.latched("Sprint"), "latched again");
    a.resetLatches();
    check(!a.latched("Sprint") && a.toggleChosen("Sprint") && frame(0.0f) == 0.0f, "reset clears the latch and keeps the choice");

    // Two actions do not share a latch.
    a.setToggle("Crouch", true);
    f32 c[3] = {1, 0, 0};
    a.filter("Crouch", true, c);
    check(a.latched("Crouch") && !a.latched("Sprint"), "latches are per action");
    a.setToggle("Crouch", false);
    check(!a.latched("Crouch"), "turning the toggle off drops the latch");
}

// ---- the trigger engine ------------------------------------------------------------------------------

struct Ctx final : fi::TriggerContext {
    std::map<std::string, int32_t> handles = {{"Fire", 1}, {"Jump", 2}};
    std::map<int32_t, bool> roseNow;
    int32_t find(const std::string& n) override { const auto it = handles.find(n); return it == handles.end() ? 0 : it->second; }
    fi::Rank stateOf(int32_t) override { return fi::Rank::None; }
    bool rose(int32_t h) override { return roseNow[h]; }
};

static fmt::OcTriggerSpec spec(const char* text) {
    fmt::OcTriggerSpec s;
    std::string err;
    if (!fmt::parseTriggerSpec(text, s, &err)) AVER_ERROR("bad spec {}: {}", text, err);
    return s;
}

static fi::V3 held() { fi::V3 v; v.c[0] = 1.0f; return v; }
static fi::V3 idle() { return fi::V3{}; }

// Steps a trigger with a held input in dt steps and returns the time it first reported Triggered, or -1.
static float timeToTrigger(const fmt::OcTriggerSpec& s, const fi::Timing* t, float limit = 3.0f) {
    fi::TriggerRuntime rt;
    Ctx ctx;
    float now = 0.0f;
    while (now < limit) {
        now += 0.05f;
        if (fi::stepTrigger(s, rt, held(), 0.05f, ctx, t) == fi::Rank::Triggered) return now;
    }
    return -1.0f;
}

static void testTiming() {
    AVER_INFO("=== timing scales ===");
    const fi::Timing quick{0.5f, 3.0f};

    const fmt::OcTriggerSpec hold = spec("hold:0.5");
    const float plain = timeToTrigger(hold, nullptr), fast = timeToTrigger(hold, &quick);
    check(plain > 0.49f && plain < 0.56f, "a 0.5 s hold fires at 0.5 s unassisted");
    check(fast > 0.24f && fast < 0.31f, "and at 0.25 s with a 0.5 hold scale");

    // HoldRelease: released after 0.3 s fires only with the scaled time.
    const fmt::OcTriggerSpec hr = spec("holdrelease:0.5");
    for (int pass = 0; pass < 2; ++pass) {
        fi::TriggerRuntime rt;
        Ctx ctx;
        const fi::Timing* t = pass ? &quick : nullptr;
        for (int i = 0; i < 6; ++i) fi::stepTrigger(hr, rt, held(), 0.05f, ctx, t);   // 0.3 s
        const fi::Rank r = fi::stepTrigger(hr, rt, idle(), 0.05f, ctx, t);
        check((r == fi::Rank::Triggered) == (pass == 1), pass ? "hold-release fires on a 0.3 s hold with the hold scale" : "and not without it");
    }

    // Tap: held 0.5 s is too long for a 0.2 s tap, but fits a 0.2 * 3 window.
    const fmt::OcTriggerSpec tap = spec("tap:0.2");
    for (int pass = 0; pass < 2; ++pass) {
        fi::TriggerRuntime rt;
        Ctx ctx;
        const fi::Timing* t = pass ? &quick : nullptr;
        for (int i = 0; i < 10; ++i) fi::stepTrigger(tap, rt, held(), 0.05f, ctx, t);   // 0.5 s
        const fi::Rank r = fi::stepTrigger(tap, rt, idle(), 0.05f, ctx, t);
        check((r == fi::Rank::Triggered) == (pass == 1), pass ? "a slow tap counts with the window scale" : "and is too slow without it");
    }

    // Combo: the second step 0.6 s after the first is outside 0.4 s but inside 0.4 * 3.
    const fmt::OcTriggerSpec combo = spec("combo:0.4:Fire,Jump");
    for (int pass = 0; pass < 2; ++pass) {
        fi::TriggerRuntime rt;
        Ctx ctx;
        const fi::Timing* t = pass ? &quick : nullptr;
        ctx.roseNow[1] = true;
        fi::stepTrigger(combo, rt, idle(), 0.05f, ctx, t);
        ctx.roseNow[1] = false;
        for (int i = 0; i < 12; ++i) fi::stepTrigger(combo, rt, idle(), 0.05f, ctx, t);   // 0.6 s
        ctx.roseNow[2] = true;
        const fi::Rank r = fi::stepTrigger(combo, rt, idle(), 0.05f, ctx, t);
        check((r == fi::Rank::Triggered) == (pass == 1), pass ? "a slow double tap lands inside the scaled combo window" : "and misses the authored window");
    }

    // Pulse, Chord and Down ignore the scales.
    const fmt::OcTriggerSpec pulse = spec("pulse:0.25");
    check(timeToTrigger(pulse, nullptr) == timeToTrigger(pulse, &quick), "pulse is untouched");
    const fmt::OcTriggerSpec down = spec("down");
    check(timeToTrigger(down, nullptr) == timeToTrigger(down, &quick), "down is untouched");
    {
        const fmt::OcTriggerSpec chord = spec("chord:Fire");
        Ctx ctx;
        fi::TriggerRuntime a, b;
        const fi::Rank r1 = fi::stepTrigger(chord, a, held(), 0.05f, ctx, nullptr), r2 = fi::stepTrigger(chord, b, held(), 0.05f, ctx, &quick);
        check(r1 == r2, "chord is untouched");
    }
}

static void testNullTimingIsOldBehaviour() {
    AVER_INFO("=== a null Timing is the old behaviour ===");
    const char* specs[] = {"down", "pressed", "released", "hold:0.4", "hold:0.4:0.15:once", "holdrelease:0.3", "tap:0.2", "pulse:0.2:3"};
    const fi::Timing identity{1.0f, 1.0f};
    bool same = true;
    for (const char* text : specs) {
        const fmt::OcTriggerSpec s = spec(text);
        fi::TriggerRuntime a, b, c;
        Ctx ctx;
        for (int i = 0; i < 40; ++i) {
            const fi::V3 v = (i % 13) < 7 ? held() : idle();
            const fi::Rank ra = fi::stepTrigger(s, a, v, 0.05f, ctx);            // the trailing parameter omitted: today's call
            const fi::Rank rb = fi::stepTrigger(s, b, v, 0.05f, ctx, nullptr);
            const fi::Rank rc = fi::stepTrigger(s, c, v, 0.05f, ctx, &identity);  // a 1/1 Timing changes nothing either
            if (ra != rb || ra != rc) same = false;
        }
    }
    check(same, "omitted, null and 1/1 timing give identical results over a held/idle pattern");

    // combineTriggers with and without.
    std::vector<fmt::OcTriggerSpec> list = {spec("hold:0.3"), spec("tap:0.2")};
    std::vector<fi::TriggerRuntime> r1, r2;
    Ctx ctx;
    bool eq = true;
    for (int i = 0; i < 30; ++i) {
        const fi::V3 v = (i % 9) < 5 ? held() : idle();
        if (fi::combineTriggers(list, r1, v, 0.05f, ctx) != fi::combineTriggers(list, r2, v, 0.05f, ctx, nullptr)) eq = false;
    }
    check(eq, "combineTriggers agrees too");
}

int main() {
    AVER_INFO("AccessAssistTest");
    testToggle();
    testTiming();
    testNullTimingIsOldBehaviour();
    AVER_INFO(g_failures == 0 ? "AccessAssistTest: PASS" : "AccessAssistTest: FAIL");
    return g_failures == 0 ? 0 : 1;
}
