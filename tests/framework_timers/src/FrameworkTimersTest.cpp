// Test for the framework timer service and event bus: ordering, pause, time dilation, cancel and add
// inside a callback, re-entrancy, payloads, and the C ABI over both. Exit code = failure count.
#include "aver/core/Log.hpp"
#include "aver/framework/EventBus.hpp"
#include "aver/framework/TimerService.hpp"
#include "aver/framework/framework_timers_abi.h"

#include <cmath>
#include <stdexcept>
#include <string>
#include <vector>

using namespace aver;
using namespace aver::fw;

static int g_checks   = 0;
static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) return;
    AVER_ERROR("   FAIL  {}", what);
    ++g_failures;
}

static bool near(double a, double b, double eps = 1e-4) { return std::fabs(a - b) <= eps; }

static std::string join(const std::vector<std::string>& v) {
    std::string s;
    for (const auto& x : v) s += x;
    return s;
}

// ---------------------------------------------------------------------------------------------
// Timers

static void testOneShot() {
    AVER_INFO("=== one-shot timer ===");
    TimerService ts;
    int fired = 0;
    TimerHandle h = ts.set(1.0f, [&](TimerHandle) { ++fired; });
    check(h != 0 && ts.isActive(h), "set returns an active handle");
    check(near(ts.remaining(h), 1.0), "remaining starts at the delay");
    ts.update(0.5f);
    check(fired == 0 && near(ts.remaining(h), 0.5), "not due after half the delay");
    ts.update(0.5f);
    check(fired == 1, "fires exactly when the delay elapses");
    check(!ts.isActive(h) && ts.count() == 0, "a one-shot is released after firing");
    ts.update(1.0f);
    check(fired == 1, "a one-shot never fires twice");
    check(ts.set(std::nanf(""), [](TimerHandle) {}) == 0, "a NaN delay is refused");
}

static void testRepeatingAndCatchUp() {
    AVER_INFO("=== repeating timer and in-frame catch-up ===");
    TimerService ts;
    int fired = 0;
    TimerOptions o;
    o.repeat = true;
    TimerHandle h = ts.set(0.1f, [&](TimerHandle) { ++fired; }, o);
    ts.update(0.35f);
    check(fired == 3, "a long frame fires a repeating timer once per elapsed interval (3)");
    ts.update(0.05f);
    check(fired == 4, "the leftover time carries into the next frame (no drift)");
    check(ts.fireCount(h) == 4, "fireCount tracks fires");

    TimerService limited;
    int n = 0;
    TimerOptions lo;
    lo.repeat = true;
    lo.maxFires = 3;
    TimerHandle lh = limited.set(0.25f, [&](TimerHandle) { ++n; }, lo);
    limited.update(1.0f);
    check(n == 3 && !limited.isActive(lh), "maxFires stops a repeating timer after that many fires");

    TimerService firstDelay;
    int f = 0;
    TimerOptions fo;
    fo.repeat = true;
    fo.firstDelay = 0.5f;
    firstDelay.set(0.25f, [&](TimerHandle) { ++f; }, fo);
    firstDelay.update(0.5f);
    check(f == 1, "firstDelay governs the first fire only");
    firstDelay.update(0.5f);
    check(f == 3, "later fires use the interval");

    TimerService cap;
    int c = 0;
    TimerOptions co;
    co.repeat = true;
    cap.set(0.01f, [&](TimerHandle) { ++c; }, co);
    cap.update(1.0f);
    check(c == TimerService::kMaxCatchUp, "catch-up is capped so a hitch cannot spiral");
    c = 0;
    cap.update(0.02f);
    check(c == 2, "the timer keeps its cadence after a capped frame");

    TimerService zero;
    int z = 0;
    TimerOptions zo;
    zo.repeat = true;
    zero.set(0.0f, [&](TimerHandle) { ++z; }, zo);
    zero.update(0.5f);
    check(z == 1, "a zero-interval repeating timer fires once per update");
    zero.update(0.5f);
    check(z == 2, "and again next update");

    TimerService step;
    int s = 0;
    step.set(1.5f, [&](TimerHandle) { ++s; });
    step.update(100.0f);
    check(s == 0, "a huge frame delta is clamped to the max step");
}

static void testOrdering() {
    AVER_INFO("=== ordering ===");
    {
        TimerService ts;
        std::vector<std::string> log;
        ts.set(0.75f, [&](TimerHandle) { log.push_back("A"); });
        ts.set(0.25f, [&](TimerHandle) { log.push_back("B"); });
        ts.set(0.5f,  [&](TimerHandle) { log.push_back("C"); });
        ts.update(1.0f);
        check(join(log) == "BCA", "timers fire in due order, not creation order");
    }
    {
        TimerService ts;
        std::vector<std::string> log;
        for (const char* name : {"1", "2", "3", "4"}) {
            std::string nm = name;
            ts.set(0.5f, [&log, nm](TimerHandle) { log.push_back(nm); });
        }
        ts.update(0.5f);
        check(join(log) == "1234", "equal due moments fire in creation order");
    }
    {
        TimerService ts;
        std::vector<std::string> log;
        TimerOptions ro;
        ro.repeat = true;
        ts.set(0.25f, [&](TimerHandle) { log.push_back("R"); }, ro);
        ts.set(0.5f,  [&](TimerHandle) { log.push_back("O"); });
        ts.update(1.0f);
        check(join(log) == "RRORR", "a repeating timer interleaves with a one-shot (tie goes to the older timer)");
    }
    {
        TimerService ts;
        std::vector<std::string> log;
        ts.set(0.5f,  [&](TimerHandle) { log.push_back("O"); });
        TimerOptions ro;
        ro.repeat = true;
        ts.set(0.25f, [&](TimerHandle) { log.push_back("R"); }, ro);
        ts.update(1.0f);
        check(join(log) == "RORRR", "the same tie goes the other way when the one-shot is older");
    }
}

static void testPauseAndDilation() {
    AVER_INFO("=== pause and time dilation ===");
    TimerService ts;
    int normal = 0, ignoring = 0;
    TimerHandle hn = ts.set(1.0f, [&](TimerHandle) { ++normal; });
    TimerOptions io;
    io.flags = kTimerIgnorePause;
    ts.set(0.5f, [&](TimerHandle) { ++ignoring; }, io);

    ts.setPaused(true);
    ts.update(0.5f);
    check(normal == 0 && near(ts.remaining(hn), 1.0), "a paused service does not advance normal timers");
    check(ignoring == 1, "an IgnorePause timer keeps running while paused");
    check(near(ts.time(), 0.0) && near(ts.realTime(), 0.5), "game time freezes while paused; real time does not");
    ts.setPaused(false);
    ts.update(0.5f);
    check(normal == 0 && near(ts.remaining(hn), 0.5), "resuming continues from the preserved remaining time");
    ts.update(0.5f);
    check(normal == 1, "and the timer then fires");

    TimerService one;
    int p = 0;
    TimerHandle hp = one.set(0.5f, [&](TimerHandle) { ++p; });
    check(one.setPaused(hp, true) && one.isPaused(hp), "a single timer can be paused");
    one.update(1.0f);
    check(p == 0 && near(one.remaining(hp), 0.5), "a paused timer holds its remaining time");
    one.setPaused(hp, false);
    one.update(0.5f);
    check(p == 1, "a resumed timer fires");

    TimerService dil;
    int slow = 0, real = 0;
    dil.setDilation(0.5f);
    dil.set(1.0f, [&](TimerHandle) { ++slow; });
    TimerOptions ro;
    ro.flags = kTimerIgnoreDilation;
    dil.set(1.0f, [&](TimerHandle) { ++real; }, ro);
    dil.update(1.0f);
    check(real == 1 && slow == 0, "half-speed time delays a normal timer; IgnoreDilation runs on real time");
    check(near(dil.time(), 0.5) && near(dil.realTime(), 1.0), "game time is scaled, real time is not");
    dil.update(1.0f);
    check(slow == 1, "the dilated timer fires after twice the real time");

    TimerService frozen;
    int fz = 0;
    frozen.set(0.25f, [&](TimerHandle) { ++fz; });
    frozen.setDilation(0.0f);
    frozen.update(1.0f);
    check(fz == 0, "dilation 0 freezes game timers");
    frozen.setDilation(1000.0f);
    check(near(frozen.dilation(), 100.0), "dilation is clamped to 100");
    frozen.setDilation(-3.0f);
    check(near(frozen.dilation(), 0.0), "negative dilation clamps to 0");
}

static void testCancelInsideCallback() {
    AVER_INFO("=== cancel and add inside a callback ===");
    {
        TimerService ts;
        std::vector<std::string> log;
        TimerHandle b = 0;
        ts.set(0.25f, [&](TimerHandle) { log.push_back("A"); ts.cancel(b); });
        b = ts.set(0.5f, [&](TimerHandle) { log.push_back("B"); });
        ts.set(0.75f, [&](TimerHandle) { log.push_back("C"); });
        ts.update(1.0f);
        check(join(log) == "AC", "a callback can cancel a timer due later in the same frame");
    }
    {
        TimerService ts;
        int n = 0;
        TimerHandle self = 0;
        TimerOptions ro;
        ro.repeat = true;
        self = ts.set(0.25f, [&](TimerHandle h) {
            ++n;
            if (n == 3) check(ts.cancel(h), "a repeating timer can cancel itself from its own callback");
        }, ro);
        ts.update(1.0f);
        check(n == 3 && !ts.isActive(self), "a self-cancelled repeating timer stops immediately, even mid-catch-up");
    }
    {
        TimerService ts;
        bool cancelResult = true;
        ts.set(0.25f, [&](TimerHandle h) { cancelResult = ts.cancel(h); });
        ts.update(0.5f);
        check(!cancelResult, "a one-shot is already released inside its own callback");
    }
    {
        TimerService ts;
        int late = 0;
        ts.set(0.25f, [&](TimerHandle) { ts.set(0.0f, [&](TimerHandle) { ++late; }); });
        ts.update(1.0f);
        check(late == 0, "a timer created inside a callback does not fire in the same update");
        ts.update(0.1f);
        check(late == 1, "it fires on the next update (zero delay = next frame)");
    }
    {
        TimerService ts;
        int n = 0;
        TimerOptions ro;
        ro.repeat = true;
        ts.set(0.25f, [&](TimerHandle) {
            ++n;
            for (int i = 0; i < 200; ++i) ts.set(5.0f, [](TimerHandle) {});   // forces the slot table to grow
        }, ro);
        ts.update(0.5f);
        check(n == 2, "growing the timer table from inside a callback does not disturb the running update");
    }
    {
        TimerService ts;
        TimerHandle t1 = ts.set(5.0f, [](TimerHandle) {});
        ts.cancel(t1);
        TimerHandle t2 = ts.set(5.0f, [](TimerHandle) {});
        check(t1 != t2 && !ts.isActive(t1) && ts.isActive(t2), "a stale handle never aliases a reused slot");
        check(!ts.cancel(t1) && ts.isActive(t2), "cancelling a stale handle is a no-op");
    }
    {
        TimerService ts;
        int fired = 0;
        TimerOptions o;
        o.owner = 7;
        ts.set(1.0f, [&](TimerHandle) { ++fired; }, o);
        ts.set(1.0f, [&](TimerHandle) { ++fired; }, o);
        ts.set(1.0f, [&](TimerHandle) { ++fired; });
        check(ts.cancelOwner(0) == 0, "owner 0 is not a wildcard");
        check(ts.cancelOwner(7) == 2 && ts.count() == 1, "cancelOwner removes only that owner's timers");
        ts.update(1.0f);
        check(fired == 1, "the unowned timer survives");
    }
    {
        TimerService ts;
        int ok = 0;
        ts.set(0.25f, [&](TimerHandle) { throw std::runtime_error("boom"); });
        ts.set(0.5f,  [&](TimerHandle) { ++ok; });
        ts.update(1.0f);
        check(ok == 1 && ts.errors() == 1, "an exception in a callback is contained and later timers still run");
    }
    {
        TimerService ts;
        int fired = 0;
        TimerHandle h = ts.set(1.0f, [&](TimerHandle) { ++fired; });
        ts.update(0.75f);
        check(ts.reset(h), "reset accepts a live handle");
        ts.update(0.75f);
        check(fired == 0, "reset restarts the countdown");
        ts.update(0.25f);
        check(fired == 1, "and the timer fires one full delay later");
    }
    {
        TimerService ts;
        TimerHandle b = 0;
        int fired = 0;
        ts.set(0.25f, [&](TimerHandle) { ts.reset(b, 5.0f); });
        b = ts.set(0.5f, [&](TimerHandle) { ++fired; });
        ts.update(1.0f);
        check(fired == 0, "a callback that resets a due timer pushes it back");
    }
}

// ---------------------------------------------------------------------------------------------
// Event bus

static Event makeEvent(const char* name, int32_t sender = 0, int32_t target = 0) {
    Event e;
    e.name = name;
    e.sender = sender;
    e.target = target;
    return e;
}

static void testBusOrdering() {
    AVER_INFO("=== event bus: ordering and consume ===");
    EventBus bus;
    std::vector<std::string> log;
    SubscribeOptions lo, mid, hi;
    lo.priority = -1;
    hi.priority = 10;
    bus.subscribe("e", [&](const Event&) { log.push_back("m1"); return false; }, mid);
    bus.subscribe("e", [&](const Event&) { log.push_back("lo"); return false; }, lo);
    bus.subscribe("e", [&](const Event&) { log.push_back("hi"); return false; }, hi);
    bus.subscribe("e", [&](const Event&) { log.push_back("m2"); return false; }, mid);
    int ran = bus.dispatch(makeEvent("e"));
    check(join(log) == "him1m2lo" && ran == 4, "higher priority first, ties in subscription order");
    check(bus.dispatch(makeEvent("none")) == 0, "an event nobody hears runs zero handlers");

    EventBus eat;
    int after = 0;
    SubscribeOptions first;
    first.priority = 1;
    eat.subscribe("e", [&](const Event&) { return true; }, first);
    eat.subscribe("e", [&](const Event&) { ++after; return false; });
    check(eat.dispatch(makeEvent("e")) == 1 && after == 0, "a handler returning true consumes the event");
}

static void testBusSubscribeUnsubscribeInside() {
    AVER_INFO("=== event bus: subscribe and unsubscribe inside a handler ===");
    EventBus bus;
    std::vector<std::string> log;
    SubscriptionId h3 = 0, selfId = 0;
    SubscribeOptions hi, mid, lo, selfOpt;
    hi.priority = 10;
    mid.priority = 5;
    selfOpt.priority = 20;
    bus.subscribe("e", [&](const Event&) { log.push_back("H1"); bus.unsubscribe(h3); return false; }, hi);
    bus.subscribe("e", [&](const Event&) { log.push_back("H2"); return false; }, mid);
    h3 = bus.subscribe("e", [&](const Event&) { log.push_back("H3"); return false; }, lo);
    bus.dispatch(makeEvent("e"));
    check(join(log) == "H1H2", "a subscriber removed by an earlier handler is skipped in the same dispatch");
    check(bus.listenerCount("e") == 2, "and is gone afterwards");

    log.clear();
    int selfCalls = 0;
    selfId = bus.subscribe("e", [&](const Event&) { ++selfCalls; bus.unsubscribe(selfId); return false; }, selfOpt);
    bus.dispatch(makeEvent("e"));
    bus.dispatch(makeEvent("e"));
    check(selfCalls == 1, "a handler can unsubscribe itself");

    EventBus adds;
    int late = 0;
    bool added = false;
    adds.subscribe("e", [&](const Event&) {
        if (!added) { added = true; adds.subscribe("e", [&](const Event&) { ++late; return false; }); }
        return false;
    });
    adds.dispatch(makeEvent("e"));
    check(late == 0, "a subscriber added during a dispatch does not hear that event");
    adds.dispatch(makeEvent("e"));
    check(late == 1, "but hears the next one");
    check(adds.listenerCount("e") == 2, "listenerCount sees it");

    EventBus own;
    own.subscribe("e", [](const Event&) { return false; }, SubscribeOptions{5, 0, 0});
    own.subscribe("f", [](const Event&) { return false; }, SubscribeOptions{5, 0, 0});
    own.subscribe("e", [](const Event&) { return false; }, SubscribeOptions{6, 0, 0});
    check(own.unsubscribeOwner(0) == 0, "owner 0 is not a wildcard");
    check(own.unsubscribeOwner(5) == 2 && own.listenerCount("e") == 1 && own.listenerCount("f") == 0,
          "unsubscribeOwner removes that owner's subscriptions across events");

    EventBus clr;
    int cleared = 0;
    clr.subscribe("e", [&](const Event&) { ++cleared; clr.clear(); return false; });
    clr.subscribe("e", [&](const Event&) { ++cleared; return false; });
    clr.dispatch(makeEvent("e"));
    check(cleared == 1 && clr.subscriberCount() == 0, "clear() from inside a handler stops the rest of that dispatch");
}

static void testBusReentrancy() {
    AVER_INFO("=== event bus: re-entrancy ===");
    EventBus bus;
    int count = 0;
    bus.subscribe("ping", [&](const Event&) {
        ++count;
        if (count < 20) bus.dispatch(makeEvent("ping"));
        return false;
    });
    bus.dispatch(makeEvent("ping"));
    check(count == bus.maxDepth(), "recursive dispatch stops at the nesting limit instead of overflowing the stack");
    check(bus.overflowed() == 1 && bus.pending() == 1, "the event past the limit is queued, not dropped");
    bus.flush();
    check(count == 20 && bus.pending() == 0, "flush delivers the demoted event and the chain completes");

    EventBus nest;
    std::vector<std::string> log;
    nest.subscribe("a", [&](const Event&) { log.push_back("a>"); nest.dispatch(makeEvent("b")); log.push_back("<a"); return false; });
    nest.subscribe("b", [&](const Event&) { log.push_back("b"); return false; });
    nest.dispatch(makeEvent("a"));
    check(join(log) == "a>b<a", "an immediate dispatch from a handler runs to completion first");

    EventBus cur;
    std::string seen;
    cur.subscribe("outer", [&](const Event&) {
        cur.dispatch(makeEvent("inner"));
        seen += cur.current() ? cur.current()->name : "?";
        return false;
    });
    cur.subscribe("inner", [&](const Event&) { return false; });
    cur.dispatch(makeEvent("outer"));
    check(seen == "outer" && cur.current() == nullptr, "current() is restored after a nested dispatch and null outside");
}

static void testBusDeferred() {
    AVER_INFO("=== event bus: post and flush ===");
    EventBus bus;
    std::vector<std::string> log;
    bus.subscribe("e", [&](const Event& e) {
        log.push_back(e.name + std::to_string(e.sender));
        if (e.sender == 1) bus.post(makeEvent("e", 4));
        return false;
    });
    bus.post(makeEvent("e", 1));
    bus.post(makeEvent("e", 2));
    bus.post(makeEvent("e", 3));
    check(log.empty() && bus.pending() == 3, "post does not deliver until flush");
    int n = bus.flush();
    check(join(log) == "e1e2e3e4" && n == 4, "flush is FIFO and an event posted by a handler runs in the next pass");
    check(bus.pending() == 0, "the queue is empty afterwards");

    EventBus runaway;
    int runs = 0;
    runaway.subscribe("loop", [&](const Event&) { ++runs; runaway.post(makeEvent("loop")); return false; });
    runaway.post(makeEvent("loop"));
    int done = runaway.flush();
    check(done == runaway.flushPasses() && runs == done, "a handler that always re-posts is bounded to one event per pass");
    check(runaway.pending() == 1, "the remainder waits for the next flush");

    EventBus inside;
    int inner = -1;
    inside.subscribe("e", [&](const Event&) { inner = inside.flush(); return false; });
    inside.post(makeEvent("e"));
    inside.flush();
    check(inner == 0, "flush called from inside a handler is refused");

    EventBus cleared;
    int c = 0;
    cleared.subscribe("e", [&](const Event&) { ++c; cleared.clearPending(); return false; });
    cleared.post(makeEvent("e"));
    cleared.post(makeEvent("e"));
    cleared.flush();
    check(c == 1, "clearPending from inside a handler abandons the rest of the batch");
}

static void testBusPayloadAndFilters() {
    AVER_INFO("=== event bus: payload, sender and target filters ===");
    EventBus bus;
    int64_t i = 0;
    float f = 0, fi = 0;
    bool b = false;
    std::string s;
    float v[3] = {0, 0, 0};
    int32_t ent = 0;
    size_t count = 0;
    int64_t floatAsInt = 0, strAsInt = -1;
    bus.subscribe("p", [&](const Event& e) {
        const auto& a = e.payload.args;
        count = a.size();
        i = a[0].asInt();
        f = a[1].asFloat();
        b = a[2].asBool();
        s = a[3].s;
        v[0] = a[4].f[0]; v[1] = a[4].f[1]; v[2] = a[4].f[2];
        ent = static_cast<int32_t>(a[5].asInt());
        fi = a[0].asFloat();
        floatAsInt = a[1].asInt();
        strAsInt = a[3].asInt();
        return false;
    });
    Event e = makeEvent("p", 3, 0);
    e.payload.push(EventValue::ofInt(5));
    e.payload.push(EventValue::ofFloat(2.5f));
    e.payload.push(EventValue::ofBool(true));
    e.payload.push(EventValue::ofString("hi"));
    e.payload.push(EventValue::ofVec3(1, 2, 3));
    e.payload.push(EventValue::ofEntity(9));
    bus.dispatch(e);
    check(count == 6 && i == 5 && near(f, 2.5) && b && s == "hi" && v[0] == 1 && v[2] == 3 && ent == 9,
          "every payload kind round-trips");
    check(near(fi, 5.0) && floatAsInt == 2 && strAsInt == 0, "numeric kinds coerce into each other; a string reads 0");

    EventPayload full;
    bool ok = true;
    for (size_t k = 0; k < EventPayload::kMaxArgs; ++k) ok = ok && full.push(EventValue::ofInt(1));
    check(ok && !full.push(EventValue::ofInt(1)), "the payload is capped");

    EventBus filt;
    std::vector<std::string> log;
    filt.subscribe("e", [&](const Event&) { log.push_back("any"); return false; });
    filt.subscribe("e", [&](const Event&) { log.push_back("s5"); return false; }, SubscribeOptions{0, 0, 5});
    filt.subscribe("e", [&](const Event&) { log.push_back("o1"); return false; }, SubscribeOptions{1, 0, 0});
    filt.subscribe("e", [&](const Event&) { log.push_back("o2"); return false; }, SubscribeOptions{2, 0, 0});
    filt.dispatch(makeEvent("e", 5, 0));
    check(join(log) == "anys5o1o2", "a broadcast reaches every subscriber whose sender filter matches");
    log.clear();
    filt.dispatch(makeEvent("e", 6, 0));
    check(join(log) == "anyo1o2", "a sender filter excludes other senders");
    log.clear();
    filt.dispatch(makeEvent("e", 5, 2));
    check(join(log) == "o2", "a targeted event reaches only subscribers owned by the target");

    EventBus thrower;
    int after = 0;
    SubscribeOptions hi;
    hi.priority = 1;
    thrower.subscribe("e", [&](const Event&) -> bool { throw std::runtime_error("x"); }, hi);
    thrower.subscribe("e", [&](const Event&) { ++after; return false; });
    thrower.dispatch(makeEvent("e"));
    check(after == 1 && thrower.errors() == 1, "an exception in a handler is contained and later handlers run");
    check(thrower.subscribe("", [](const Event&) { return false; }) == 0, "an empty event name is refused");
}

// ---------------------------------------------------------------------------------------------
// C ABI

namespace {
struct AbiCtx {
    std::vector<std::string> timerLog;
    std::vector<std::string> eventLog;
    int32_t consume = 0;
    int64_t i = 0;
    float f = 0, v[3] = {0, 0, 0};
    std::string s;
    int32_t b = 0, ent = 0, argCount = -1, validInside = 0;
};
AbiCtx g_ctx;

void AVER_FW_CALL onTimer(int32_t handle, int64_t token, void* user) {
    static_cast<AbiCtx*>(user)->timerLog.push_back(std::to_string(token) + (handle != 0 ? "" : "!"));
}

int32_t AVER_FW_CALL onEvent(int32_t sub, int64_t token, void* user) {
    AbiCtx* c = static_cast<AbiCtx*>(user);
    c->eventLog.push_back(std::string(aver_fw_event_current_name()) + ":" + std::to_string(token));
    c->validInside = aver_fw_event_current_valid();
    c->argCount = aver_fw_event_current_arg_count();
    if (c->argCount >= 6) {
        c->i   = aver_fw_event_current_arg_int(0);
        c->f   = aver_fw_event_current_arg_float(1);
        c->b   = aver_fw_event_current_arg_bool(2);
        c->s   = aver_fw_event_current_arg_string(3);
        aver_fw_event_current_arg_vec3(4, c->v);
        c->ent = aver_fw_event_current_arg_entity(5);
    }
    (void)sub;
    return c->consume;
}
} // namespace

static void testAbiTimers() {
    AVER_INFO("=== C ABI: timers ===");
    check(aver_fw_timers_abi_version() == AVER_FW_TIMERS_ABI_VERSION, "ABI version reports");
    g_ctx = AbiCtx{};
    aver_fw_timers_clear();
    aver_fw_timers_set_paused(0);
    aver_fw_timers_set_dilation(1.0f);
    aver_fw_set_timer_dispatch(onTimer, &g_ctx);

    const int32_t epoch0 = aver_fw_timers_epoch();
    int32_t h = aver_fw_timer_set(0.5f, -1.0f, 0, 0, 3, 77);
    check(h != 0 && aver_fw_timer_active(h) == 1 && aver_fw_timers_count() == 1, "timer_set returns a live handle");
    aver_fw_timers_update(0.25f);
    check(g_ctx.timerLog.empty() && std::fabs(aver_fw_timer_remaining(h) - 0.25f) < 1e-4f, "not due yet");
    aver_fw_timers_update(0.25f);
    check(g_ctx.timerLog.size() == 1 && g_ctx.timerLog[0] == "77", "fires with its token");
    check(aver_fw_timer_active(h) == 0 && aver_fw_timer_fire_count(h) == -1, "a fired one-shot is released");

    g_ctx.timerLog.clear();
    int32_t r = aver_fw_timer_set(0.25f, -1.0f, 2, AVER_FW_TIMER_REPEAT, 0, 5);
    aver_fw_timers_update(1.0f);
    check(g_ctx.timerLog.size() == 2 && aver_fw_timer_active(r) == 0, "a repeating timer honours maxFires");

    g_ctx.timerLog.clear();
    aver_fw_timers_set_dilation(2.0f);
    check(std::fabs(aver_fw_timers_dilation() - 2.0f) < 1e-6f, "dilation reads back");
    check(std::fabs(aver_fw_timers_scaled_dt(0.5f) - 1.0f) < 1e-6f, "scaled_dt applies dilation");
    aver_fw_timer_set(1.0f, -1.0f, 0, 0, 0, 9);
    aver_fw_timers_update(0.5f);
    check(g_ctx.timerLog.size() == 1, "dilation 2 makes a 1s timer fire after 0.5s");
    aver_fw_timers_set_dilation(1.0f);

    g_ctx.timerLog.clear();
    aver_fw_timers_set_paused(1);
    check(aver_fw_timers_paused() == 1 && aver_fw_timers_scaled_dt(0.5f) == 0.0f, "paused reads back and zeroes scaled_dt");
    aver_fw_timer_set(0.25f, -1.0f, 0, 0, 0, 1);
    aver_fw_timer_set(0.25f, -1.0f, 0, AVER_FW_TIMER_IGNORE_PAUSE, 0, 2);
    aver_fw_timers_update(0.5f);
    check(g_ctx.timerLog.size() == 1 && g_ctx.timerLog[0] == "2", "only the IgnorePause timer runs while paused");
    aver_fw_timers_set_paused(0);
    aver_fw_timers_update(0.5f);
    check(g_ctx.timerLog.size() == 2 && g_ctx.timerLog[1] == "1", "the other runs after resume");

    aver_fw_timers_clear();
    int32_t o1 = aver_fw_timer_set(1.0f, -1.0f, 0, 0, 8, 1);
    aver_fw_timer_set(1.0f, -1.0f, 0, 0, 8, 2);
    aver_fw_timer_set(1.0f, -1.0f, 0, 0, 9, 3);
    check(aver_fw_timer_cancel_owner(0) == 0 && aver_fw_timer_cancel_owner(8) == 2, "cancel_owner counts, owner 0 is a no-op");
    check(aver_fw_timer_cancel(o1) == 0, "a stale handle cancels nothing");
    check(aver_fw_timers_epoch() != epoch0, "clear bumps the epoch so a managed layer drops its records");
    check(aver_fw_timers_time_us() > 0 && aver_fw_timers_real_time_us() >= aver_fw_timers_time_us() / 2,
          "the clocks accumulate");
    aver_fw_timers_clear();
    aver_fw_set_timer_dispatch(nullptr, nullptr);
}

static void testAbiEvents() {
    AVER_INFO("=== C ABI: events ===");
    g_ctx = AbiCtx{};
    aver_fw_events_clear();
    aver_fw_set_event_dispatch(onEvent, &g_ctx);

    check(aver_fw_event_subscribe("", 0, 0, 0, 1) == 0, "an empty name is refused");
    int32_t s1 = aver_fw_event_subscribe("hit", 4, 0, 0, 11);
    check(s1 != 0 && aver_fw_event_listener_count("hit") == 1, "subscribe returns an id");

    aver_fw_event_push_int(5);
    aver_fw_event_push_float(2.5f);
    aver_fw_event_push_bool(1);
    aver_fw_event_push_string("hello");
    aver_fw_event_push_vec3(1.0f, 2.0f, 3.0f);
    aver_fw_event_push_entity(42);
    check(aver_fw_event_dispatch("hit", 7, 0) == 1, "dispatch reports one handler");
    check(g_ctx.eventLog.size() == 1 && g_ctx.eventLog[0] == "hit:11" && g_ctx.validInside == 1, "the handler sees name and token");
    check(g_ctx.argCount == 6 && g_ctx.i == 5 && std::fabs(g_ctx.f - 2.5f) < 1e-6f && g_ctx.b == 1 && g_ctx.s == "hello"
              && g_ctx.v[0] == 1.0f && g_ctx.v[1] == 2.0f && g_ctx.v[2] == 3.0f && g_ctx.ent == 42,
          "the payload reads back through the accessors");
    check(aver_fw_event_current_valid() == 0 && aver_fw_event_current_arg_count() == 0, "nothing is current outside a handler");

    aver_fw_event_dispatch("hit", 0, 0);
    check(g_ctx.argCount == 0, "dispatch consumed the staged payload");

    g_ctx.eventLog.clear();
    check(aver_fw_event_post("hit", 0, 0) == 1 && aver_fw_events_pending() == 1 && g_ctx.eventLog.empty(), "post defers");
    check(aver_fw_events_flush() == 1 && g_ctx.eventLog.size() == 1 && aver_fw_events_pending() == 0, "flush delivers");

    g_ctx.eventLog.clear();
    check(aver_fw_event_dispatch("hit", 0, 99) == 0 && g_ctx.eventLog.empty(), "a targeted event skips subscribers owned by others");
    check(aver_fw_event_dispatch("hit", 0, 4) == 1, "and reaches the target's own subscriber");

    int32_t s2 = aver_fw_event_subscribe("hit", 0, 5, 0, 22);
    g_ctx.eventLog.clear();
    g_ctx.consume = 1;
    aver_fw_event_dispatch("hit", 0, 0);
    check(g_ctx.eventLog.size() == 1 && g_ctx.eventLog[0] == "hit:22", "a higher-priority handler runs first and a non-zero return consumes");
    g_ctx.consume = 0;
    check(aver_fw_event_unsubscribe(s2) == 1 && aver_fw_event_unsubscribe(s2) == 0, "unsubscribe is idempotent");
    check(aver_fw_event_unsubscribe_owner(4) == 1 && aver_fw_event_listener_count("hit") == 0, "unsubscribe_owner removes by owner");
    check(aver_fw_event_dispatch(nullptr, 0, 0) == -1 && aver_fw_event_post("", 0, 0) == -1, "a missing name is rejected");

    aver_fw_events_clear();
    aver_fw_set_event_dispatch(nullptr, nullptr);
}

int main() {
    AVER_INFO("FrameworkTimersTest");
    testOneShot();
    testRepeatingAndCatchUp();
    testOrdering();
    testPauseAndDilation();
    testCancelInsideCallback();
    testBusOrdering();
    testBusSubscribeUnsubscribeInside();
    testBusReentrancy();
    testBusDeferred();
    testBusPayloadAndFilters();
    testAbiTimers();
    testAbiEvents();
    if (g_failures == 0) AVER_INFO("=== all {} checks passed ===", g_checks);
    else AVER_ERROR("=== FAILED === {} of {} checks failed", g_failures, g_checks);
    return g_failures;
}
