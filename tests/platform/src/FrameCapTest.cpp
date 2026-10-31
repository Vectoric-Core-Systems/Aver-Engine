// frameCapWaitSeconds -- the arithmetic of the frame rate cap, checked with no clock and no waiting.
#include "aver/core/Time.hpp"
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"

#include <cmath>
#include <limits>
#include <string>

using namespace aver;

static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

static bool near(f64 a, f64 b) { return std::fabs(a - b) < 1e-9; }

int main() {
    AVER_INFO("FrameCapTest");

    check(near(frameCapWaitSeconds(0.0, 60.0), 1.0 / 60.0), "a frame that took no time waits the whole 1/60 s");
    check(near(frameCapWaitSeconds(0.010, 60.0), 1.0 / 60.0 - 0.010), "a 10 ms frame waits out the rest of 1/60 s");
    check(frameCapWaitSeconds(1.0 / 30.0, 60.0) == 0.0, "a frame already longer than the target does not wait");
    check(frameCapWaitSeconds(1.0 / 60.0, 60.0) == 0.0, "a frame exactly the target does not wait");
    check(frameCapWaitSeconds(0.001, 0.0) == 0.0, "0 fps is no cap");
    check(frameCapWaitSeconds(0.001, -5.0) == 0.0, "a negative rate is no cap");
    check(frameCapWaitSeconds(0.001, std::numeric_limits<f64>::quiet_NaN()) == 0.0, "NaN is no cap");
    check(near(frameCapWaitSeconds(0.0, 1.0), 1.0), "1 fps waits a second");
    check(frameCapWaitSeconds(0.0, 1.0e9) < 1.0e-8, "an absurdly high cap waits next to nothing");

    if (g_failures == 0) { AVER_INFO("FrameCapTest: ALL PASS"); return 0; }
    AVER_ERROR("FrameCapTest: {} failure(s)", g_failures);
    return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
}
