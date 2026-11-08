// ViewportSnapTest -- the snap rounding, step clamps and labels behind the viewport options bar.
#include "../../../sandbox/src/ViewportSnap.hpp"

#include "aver/core/Log.hpp"

#include <string>

using namespace aver;
using namespace aver::editor;

static int g_checks = 0, g_failures = 0;
static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

int main() {
    AVER_INFO("=== viewport snap ===");
    check(snapValue(13.0f, 10.0f) == 10.0f && snapValue(16.0f, 10.0f) == 20.0f, "rounds to the nearest multiple");
    check(snapValue(-13.0f, 10.0f) == -10.0f, "negative values round symmetrically");
    check(snapValue(7.3f, 0.0f) == 7.3f && snapValue(7.3f, -1.0f) == 7.3f, "a non-positive step leaves the value alone");
    check(snapValue(0.37f, 0.25f) == 0.25f, "fractional steps");
    check(clampSnapStep(SnapKind::Move, 0.0f) == 0.01f && clampSnapStep(SnapKind::Move, 1e9f) == 10000.0f, "move step clamps");
    check(clampSnapStep(SnapKind::Rotate, 500.0f) == 180.0f && clampSnapStep(SnapKind::Rotate, 0.0f) == 0.1f, "rotate step clamps");
    check(clampSnapStep(SnapKind::Scale, 99.0f) == 10.0f, "scale step clamps");
    check(snapLabel(SnapKind::Move, 10.0f) == "10" && snapLabel(SnapKind::Scale, 0.25f) == "0.25", "labels drop trailing zeros");
    check(snapLabel(SnapKind::Rotate, 15.0f) == "15\xC2\xB0", "rotation labels carry the degree sign");
    bool sorted = true;
    for (usize i = 1; i < sizeof kMoveSnapPresets / sizeof(float); ++i) sorted = sorted && kMoveSnapPresets[i] > kMoveSnapPresets[i - 1];
    check(sorted, "presets ascend");
    if (g_failures == 0) AVER_INFO("=== all {} snap checks passed ===", g_checks);
    else                 AVER_ERROR("=== {} of {} snap check(s) FAILED ===", g_failures, g_checks);
    return g_failures == 0 ? 0 : 1;
}
