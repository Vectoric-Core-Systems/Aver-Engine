// GiDrawsSettle: a draw-list change rebuilds the GI volume once the list holds still, or after the longest wait, never on
// every tick it moves. No GPU: the rule is counting.
#include "aver/voxi/GiDrawsSettle.hpp"

#include <cstdio>

using aver::u32;
using aver::u64;
using aver::voxi::GiDrawsSettle;

static int g_fail = 0;
static void check(bool ok, const char* what) {
    std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_fail;
}

int main() {
    {
        // One change, then still: held for the quiet ticks, then released.
        GiDrawsSettle s(3, 100);
        int held = 0;
        while (s.hold(7)) ++held;
        check(held == 3, "a single change waits exactly the quiet ticks (3) before rebuilding");
        check(!s.pending(), "released means nothing pending");
    }
    {
        // A list that keeps changing (streaming): released at the longest wait, not never.
        GiDrawsSettle s(3, 10);
        int held = 0;
        u64 key = 1;
        while (s.hold(key++)) ++held;
        check(held == 10, "a list changing every tick rebuilds at the longest wait (10 ticks)");
        // And again for the next wave: one rebuild per wait, not one per tick.
        held = 0;
        while (s.hold(key++)) ++held;
        check(held == 10, "the next wave waits again");
    }
    {
        // A change resets the quiet count.
        GiDrawsSettle s(3, 100);
        check(s.hold(1), "tick 1 held");
        check(s.hold(1), "tick 2 held (still 1)");
        check(s.hold(2), "a new key restarts the quiet count");
        check(s.hold(2) && s.hold(2), "two more still ticks are held");
        check(!s.hold(2), "the third still tick releases");
    }
    {
        // reset(): the gate saw the list unchanged (it moved back) or rebuilt for another reason.
        GiDrawsSettle s(3, 100);
        s.hold(5);
        s.reset();
        check(!s.pending(), "reset clears the pending change");
        int held = 0;
        while (s.hold(6)) ++held;
        check(held == 3, "after a reset a change waits the full quiet ticks again");
    }
    {
        // Zero quiet ticks: the rule is off (rebuild on the first rejecting tick, as before).
        GiDrawsSettle s(0, 100);
        check(!s.hold(9), "quiet ticks 0 never holds");
    }
    std::printf(g_fail ? "=== %d FAILED ===\n" : "=== all passed ===\n", g_fail);
    return g_fail ? 1 : 0;
}
