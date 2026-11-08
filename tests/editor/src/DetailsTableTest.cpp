// The Details table's pure half: label search, default detection and the split clamp. ImGui-free.
#include "../../../sandbox/src/DetailsTable.hpp"

#include "aver/core/Log.hpp"

#include <string>

using namespace aver;
using namespace aver::editor;

static int g_failures = 0;
static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

int main() {
    check(detailsVisibleLabel("Speed##animSpeed") == "Speed", "the visible label stops at ##");
    check(detailsVisibleLabel("Location (cm)") == "Location (cm)", "a plain label is whole");
    check(detailsVisibleLabel("##id").empty(), "an ##id label shows nothing");

    check(detailsLabelMatches("Location (cm)", ""), "an empty filter matches all");
    check(detailsLabelMatches("Location (cm)", "loc"), "case-insensitive substring");
    check(detailsLabelMatches("Location (cm)", "CM loc"), "tokens match in any order");
    check(!detailsLabelMatches("Location (cm)", "loc rot"), "every token must match");
    check(detailsLabelMatches("Roughness", "  ough "), "surrounding spaces are ignored");
    check(!detailsLabelMatches("Roughness", "metal"), "a miss hides the row");
    check(!detailsLabelMatches("", "x"), "an empty label does not match a filter");

    const f32 zero[3] = {0, 0, 0};
    const f32 a[3] = {0, 0, 0}, b[3] = {0, 0.5f, 0};
    check(!detailsDiffers(a, zero, 3), "equal values are not modified");
    check(detailsDiffers(b, zero, 3), "one component off marks the row modified");
    check(!detailsDiffers(b, zero, 1), "only the first n components are compared");
    check(!detailsDiffers(a, zero, 3) && !detailsDiffers(a, zero, 0), "n = 0 is never modified");
    const f32 tiny[1] = {0.00001f};
    check(!detailsDiffers(tiny, zero, 1), "float noise under eps is not a modification");

    check(detailsSplitPx(0.4f, 500.0f, 1.0f) == 200.0f, "the split is fraction * width");
    check(detailsSplitPx(0.01f, 500.0f, 1.0f) == 70.0f, "the label column keeps its minimum");
    check(detailsSplitPx(0.99f, 500.0f, 1.0f) == 410.0f, "the value column keeps its minimum");
    check(detailsSplitPx(0.4f, 100.0f, 1.0f) == 70.0f, "a tiny panel keeps the label minimum");
    check(detailsSplitPx(0.01f, 500.0f, 2.0f) == 140.0f, "minimums scale with DPI");

    if (g_failures == 0) AVER_INFO("=== all DetailsTable checks passed ===");
    else                 AVER_ERROR("=== {} DetailsTable check(s) FAILED ===", g_failures);
    return g_failures == 0 ? 0 : 1;
}
