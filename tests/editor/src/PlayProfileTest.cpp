// PlayProfile -- the profiler panel's Play-side numbers: the phase brackets onUpdate opens and closes,
// the frame-time smoothing, and the worst-of-120 windows. Pure and header-only (std plus
// aver/core/Types.hpp): no ImGui, no Engine, no clock, because every bracket here is handed its own
// timestamps. What this cannot show is the panel itself or the call sites in onUpdate; it pins the
// arithmetic they rely on.
#include "../../../sandbox/src/PlayProfile.hpp"

#include "aver/core/Log.hpp"

#include <cmath>
#include <string>

using namespace aver;
using editor::PlayPhase;
using editor::PlayProfile;

static int g_checks = 0, g_failures = 0;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

static bool approx(f64 a, f64 b, f64 eps = 1e-6) { return std::fabs(a - b) <= eps; }

// One millisecond in the nanoseconds the brackets take.
static constexpr u64 kMs = 1000000ull;

int main() {
    AVER_INFO("=== PlayProfile ===");

    // ---- a bracket accumulates its nanoseconds, and pieces of one phase add up --------------------
    {
        PlayProfile p;
        p.begin(PlayPhase::Vehicles, 1000 * kMs);
        p.end(PlayPhase::Vehicles, 1002 * kMs);          // vehicles before the physics step: 2 ms
        p.begin(PlayPhase::Vehicles, 1010 * kMs);
        p.end(PlayPhase::Vehicles, 1013 * kMs);          // and after it: 3 ms
        p.begin(PlayPhase::Gameplay, 1003 * kMs);
        p.end(PlayPhase::Gameplay, 1010 * kMs);          // the step and the groups between: 7 ms
        p.endFrame(1.0 / 60.0, true);
        check(approx(p.phaseEmaMs(PlayPhase::Vehicles), 5.0), "two brackets of one phase in a frame sum (2 + 3 ms)");
        check(approx(p.phaseEmaMs(PlayPhase::Gameplay), 7.0), "a different phase keeps its own total");
        check(approx(p.phaseEmaMs(PlayPhase::Skinned), 0.0), "a phase nobody opened reads zero");
        check(p.playFrames() == 1, "one Play frame folded");
    }

    // ---- smoothing: the first frame seeds, later frames move by 1 - exp(-dt / tau) ----------------
    {
        PlayProfile p;
        p.begin(PlayPhase::Gameplay, 0);
        p.end(PlayPhase::Gameplay, 4 * kMs);
        p.endFrame(0.010, true);
        check(approx(p.frameEmaMs(), 10.0), "the first frame seeds the frame-time average");
        check(approx(p.phaseEmaMs(PlayPhase::Gameplay), 4.0), "and the first Play frame seeds each phase");

        p.begin(PlayPhase::Gameplay, 0);
        p.end(PlayPhase::Gameplay, 8 * kMs);
        p.endFrame(0.5, true);
        const f64 alpha = 1.0 - std::exp(-0.5 / PlayProfile::kSmoothSec);
        check(approx(p.frameEmaMs(), 10.0 + alpha * (500.0 - 10.0)), "a 0.5 s frame pulls the frame average by 1 - exp(-0.5/tau)");
        check(approx(p.phaseEmaMs(PlayPhase::Gameplay), 4.0 + alpha * (8.0 - 4.0)), "a phase moves by the same weight");
        check(approx(p.lastFrameMs(), 500.0), "the last frame alone is kept too");
    }

    // ---- outside Play the frame is smoothed but the phases are not folded --------------------------
    {
        PlayProfile p;
        p.begin(PlayPhase::ObjectAnim, 0);
        p.end(PlayPhase::ObjectAnim, 3 * kMs);           // the edit-mode animation tick
        p.endFrame(0.016, false);
        check(approx(p.frameEmaMs(), 16.0), "an edit frame still feeds the frame-time average");
        check(p.playFrames() == 0 && approx(p.phaseEmaMs(PlayPhase::ObjectAnim), 0.0),
              "but it is not a Play frame and folds no phase");
        p.endFrame(0.016, true);                         // the first Play frame, nothing timed in it
        check(approx(p.phaseEmaMs(PlayPhase::ObjectAnim), 0.0),
              "the edit frame's time is not inherited by the next Play frame");
    }

    // ---- a bracket that goes wrong loses one sample and nothing else ------------------------------
    {
        PlayProfile p;
        p.end(PlayPhase::Skinned, 10 * kMs);             // an end with no begin
        p.endFrame(0.016, true);
        check(approx(p.phaseEmaMs(PlayPhase::Skinned), 0.0), "an end() with no begin() is ignored");

        p.begin(PlayPhase::Skinned, 100 * kMs);          // an early return skipped its end()
        p.begin(PlayPhase::Skinned, 200 * kMs);          // the next begin replaces it
        p.end(PlayPhase::Skinned, 203 * kMs);
        p.end(PlayPhase::Skinned, 300 * kMs);            // a second end() is ignored
        p.endFrame(0.016, true);
        check(approx(p.phaseWorstMs(PlayPhase::Skinned), 3.0),
              "a begin() with no end() is replaced by the next begin(), and a double end() adds nothing");

        p.begin(PlayPhase::Skinned, 50 * kMs);
        p.end(PlayPhase::Skinned, 40 * kMs);             // a clock that went backwards
        p.endFrame(0.016, true);
        check(approx(p.phaseWorstMs(PlayPhase::Skinned), 3.0), "an end before its begin adds zero, never a wrapped huge value");

        p.begin(PlayPhase::Gameplay, 0);                 // still open when the frame ends
        p.endFrame(0.016, true);
        p.end(PlayPhase::Gameplay, 9 * kMs);
        p.endFrame(0.016, true);
        check(approx(p.phaseWorstMs(PlayPhase::Gameplay), 0.0), "a bracket left open across endFrame() is dropped, not charged to the next frame");
    }

    // ---- a frame with no time is skipped, and still empties the brackets --------------------------
    {
        PlayProfile p;
        p.begin(PlayPhase::Gameplay, 0);
        p.end(PlayPhase::Gameplay, 5 * kMs);
        p.endFrame(0.0, true);
        check(p.playFrames() == 0 && p.windowFrames() == 0, "dt <= 0 folds nothing");
        p.endFrame(0.016, true);
        check(approx(p.phaseEmaMs(PlayPhase::Gameplay), 0.0), "and its brackets are gone, not carried into the next frame");
    }

    // ---- the worst-of window: a spike ages out after 120 frames, a recent one does not ------------
    {
        PlayProfile p;
        for (int f = 0; f < 130; ++f) {
            const bool early = f == 1, late = f == 100;
            const f64 dt = early ? 0.200 : (late ? 0.050 : 0.010);
            p.begin(PlayPhase::Gameplay, 0);
            p.end(PlayPhase::Gameplay, (early ? 90 : (late ? 40 : 2)) * kMs);
            p.endFrame(dt, true);
        }
        check(p.windowFrames() == PlayProfile::kWindow, "the window holds kWindow frames once full");
        check(approx(p.worstFrameMs(), 50.0, 1e-3), "the spike from frame 1 has aged out; the one from frame 100 has not");
        check(approx(p.phaseWorstMs(PlayPhase::Gameplay), 40.0, 1e-3), "a phase's worst follows the same window");
    }
    {
        PlayProfile p;
        for (int f = 0; f < 5; ++f) p.endFrame(f == 2 ? 0.030 : 0.010, false);
        check(p.windowFrames() == 5 && approx(p.worstFrameMs(), 30.0, 1e-3), "before it fills, worst is over the frames there are");
    }

    // ---- fixed physics steps ----------------------------------------------------------------------
    {
        PlayProfile p;
        p.addPhysicsSteps(3);
        p.addPhysicsSteps(-2);                           // a negative count is not a step
        p.addPhysicsSteps(1);
        p.endFrame(0.016, true);
        check(p.stepsWorst() == 4 && approx(p.stepsEma(), 4.0), "steps add up within a frame and seed the average");
        p.endFrame(0.5, true);                           // a frame in which none ran
        const f64 alpha = 1.0 - std::exp(-0.5 / PlayProfile::kSmoothSec);
        check(approx(p.stepsEma(), 4.0 + alpha * (0.0 - 4.0)), "a frame with no steps pulls the average down; the count did not carry over");
        check(p.stepsWorst() == 4, "while the worst stays in the window");
        p.addPhysicsSteps(1000);
        p.endFrame(0.016, true);
        check(p.stepsWorst() == 255, "a huge catch-up saturates the byte it is stored in instead of wrapping");
    }

    // ---- resets -------------------------------------------------------------------------------------
    {
        PlayProfile p;
        p.begin(PlayPhase::Gameplay, 0);
        p.end(PlayPhase::Gameplay, 30 * kMs);
        p.addPhysicsSteps(2);
        p.endFrame(0.016, true);
        p.resetPhases();
        check(p.playFrames() == 0 && approx(p.phaseEmaMs(PlayPhase::Gameplay), 0.0) &&
              approx(p.phaseWorstMs(PlayPhase::Gameplay), 0.0) && p.stepsWorst() == 0 && approx(p.stepsEma(), 0.0),
              "resetPhases() clears the table, its worst window and the step counts");
        check(approx(p.frameEmaMs(), 16.0) && p.windowFrames() == 1, "and leaves the frame-time numbers alone");
        p.begin(PlayPhase::Gameplay, 0);
        p.end(PlayPhase::Gameplay, 10 * kMs);
        p.endFrame(0.016, true);
        check(approx(p.phaseEmaMs(PlayPhase::Gameplay), 10.0), "the next Play frame seeds afresh rather than blending with the old session");

        p.resetFrames();
        check(approx(p.frameEmaMs(), 0.0) && approx(p.worstFrameMs(), 0.0) && p.windowFrames() == 0,
              "resetFrames() clears the frame-time average and its window");
        check(p.playFrames() == 1, "and leaves the phase table alone");
        p.endFrame(0.020, false);
        check(approx(p.frameEmaMs(), 20.0), "the next frame seeds the frame-time average afresh");
    }

    if (g_failures == 0) AVER_INFO("=== all {} PlayProfile checks passed ===", g_checks);
    else                 AVER_ERROR("=== {} of {} PlayProfile check(s) FAILED ===", g_failures, g_checks);
    return g_failures == 0 ? 0 : 1;
}
