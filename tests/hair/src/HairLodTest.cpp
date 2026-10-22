// The distance rules of strand hair (HairLod.hpp): how often a groom is simulated and which strand-count level is drawn. Pure arithmetic: no scene,
// no physics, no device. Exit code = failure count.
//
// NOTHING IN HERE HAS BEEN RUN: thresholds are starting values for the first pass.
#include "aver/hair/HairLod.hpp"

#include "aver/core/Log.hpp"

#include <cmath>
#include <string>

using namespace aver;
using namespace aver::hair;

static int g_checks = 0, g_failures = 0;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    AVER_ERROR("  FAIL  {}", what);
    ++g_failures;
}

int main() {
    const HairCadenceOptions opt;
    constexpr f32 dt = 1.0f / 60.0f;

    AVER_INFO("=== cadence ===");
    {
        HairCadenceState st;
        bool everyFrame = true;
        for (int i = 0; i < 30; ++i) {
            const HairCadenceResult r = stepHairCadence(opt, st, 300.0f, true, dt);
            everyFrame = everyFrame && r.update && std::fabs(r.dt - dt) < 1e-6f && !r.teleport && r.level == HairCadence::Every;
        }
        check(everyFrame, "near the view: updated every frame with the frame's own time");
    }
    {
        HairCadenceState st;
        u32 updates = 0;
        f32 timeFed = 0.0f;
        bool halfOnly = true;
        for (int i = 0; i < 60; ++i) {
            const HairCadenceResult r = stepHairCadence(opt, st, 1500.0f, true, dt);
            halfOnly = halfOnly && r.level == HairCadence::Half && !r.teleport;
            if (r.update) { ++updates; timeFed += r.dt; }
        }
        check(halfOnly, "in the middle band: the Half level and no teleport");
        check(updates == 30, "in the middle band: every second frame (" + std::to_string(updates) + " of 60)");
        check(std::fabs(timeFed - 1.0f) < 2.0f * dt, "in the middle band: the skipped frames' time is handed to the next update (" + std::to_string(timeFed) + " s of 1)");
    }
    {
        HairCadenceState st;
        bool frozen = true;
        for (int i = 0; i < 10; ++i) {
            const HairCadenceResult r = stepHairCadence(opt, st, 3000.0f, true, dt);
            frozen = frozen && !r.update && r.level == HairCadence::Frozen;
        }
        check(frozen, "beyond the far distance: no update, level Frozen");
    }
    {
        // A long freeze, then back in range: the first update after it asks for a teleport; a short freeze does not.
        HairCadenceState st;
        for (int i = 0; i < 90; ++i) stepHairCadence(opt, st, 3000.0f, true, dt);   // 1.5 s frozen
        const HairCadenceResult back = stepHairCadence(opt, st, 300.0f, true, dt);
        check(back.update && back.teleport, "resuming after more than a second teleports");
        const HairCadenceResult next = stepHairCadence(opt, st, 300.0f, true, dt);
        check(next.update && !next.teleport, "and only that once");

        HairCadenceState st2;
        for (int i = 0; i < 20; ++i) stepHairCadence(opt, st2, 3000.0f, true, dt);  // a third of a second
        const HairCadenceResult shortBack = stepHairCadence(opt, st2, 300.0f, true, dt);
        check(shortBack.update && !shortBack.teleport, "resuming after a short freeze does not");
    }
    {
        // Not visible for long enough freezes the groom wherever it is.
        HairCadenceState st;
        bool updatedWhileHidden = false;
        for (int i = 0; i < 150; ++i) {
            const HairCadenceResult r = stepHairCadence(opt, st, 100.0f, false, dt);
            if (i > 125 && r.update) updatedWhileHidden = true;   // past 2 s
        }
        check(!updatedWhileHidden, "not visible for two seconds: frozen even when close");
        const HairCadenceResult seen = stepHairCadence(opt, st, 100.0f, true, dt);
        check(seen.update, "visible again: simulated at once");
    }

    AVER_INFO("=== level of detail ===");
    {
        const f32 d[3] = {300.0f, 700.0f, 1500.0f};
        check(chooseHairLod(100.0f, d, 0.0f, 0) == 0, "close: level 0");
        check(chooseHairLod(2000.0f, d, 0.0f, 0) == 3, "far: level 3 (skips the middle levels in one call)");
        check(chooseHairLod(500.0f, d, 0.0f, 0) == 1, "between the first two thresholds: level 1");

        // Hysteresis: at 310 cm a head that was at level 0 stays there (the move away needs 300 * 1.15), one that was at level 1 stays at 1.
        check(chooseHairLod(310.0f, d, 0.0f, 0) == 0, "just past a threshold: the level it was at holds going away");
        check(chooseHairLod(310.0f, d, 0.0f, 1) == 1, "just past a threshold: and holds coming closer");
        check(chooseHairLod(350.0f, d, 0.0f, 0) == 1, "well past it: the level changes");
        check(chooseHairLod(280.0f, d, 0.0f, 1) == 1, "just inside a threshold coming closer: holds");
        check(chooseHairLod(250.0f, d, 0.0f, 1) == 0, "well inside: changes");

        // The sequence of a head walking out and back never flickers at the boundary.
        u32 level = 0, changes = 0;
        for (int i = 0; i < 200; ++i) {
            const f32 dist = 300.0f + 20.0f * std::sin(static_cast<f32>(i) * 0.7f);   // hovering on the 300 cm boundary
            const u32 n = chooseHairLod(dist, d, 0.0f, level);
            if (n != level) { ++changes; level = n; }
        }
        check(changes == 0, "hovering on a boundary: no level changes (" + std::to_string(changes) + ")");

        check(biasedHairLod(1, 0.0f) == 1 && biasedHairLod(1, 1.0f) == 2 && biasedHairLod(1, -2.0f) == 0 && biasedHairLod(3, 5.0f) == 3, "the bias shifts the drawn level and clamps");
    }
    {
        const f32 fr[4] = {1.0f, 0.5f, 0.25f, 0.12f};
        check(std::fabs(hairDrawFraction(fr, 0, 0.0f) - 1.0f) < 1e-6f, "density 0 reads as 1");
        check(std::fabs(hairDrawFraction(fr, 1, 0.5f) - 0.25f) < 1e-6f, "density scales the level's fraction");
        check(hairDrawFraction(fr, 3, 0.01f) >= 0.01f, "the fraction has a floor");
        check(hairDrawFraction(fr, 0, 3.0f) <= 1.0f, "and never exceeds 1");
    }

    AVER_INFO("{} checks, {} failures", g_checks, g_failures);
    return g_failures;
}
