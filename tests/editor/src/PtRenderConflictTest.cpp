// PtRenderConflictTest -- the Path Tracing page's Quality-combo tag priority (PtRenderConflict.hpp's
// choosePtViewTag). Header-only and dependency-free: no ImGui, no SandboxApp, no voxi::Renderer.
#include "PtRenderConflict.hpp"

#include <cstdio>

using namespace aver::editor;

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool ok, const char* what) {
    ++g_checks;
    if (ok) {
        std::printf("[INFO ]   ok    %s\n", what);
    } else {
        ++g_failures;
        std::printf("[ERROR]   FAIL  %s\n", what);
    }
}

} // namespace

int main() {
    std::printf("[INFO ] === pt/ray-driven conflict rules ===\n");

    // ---- A1: choosePtViewTag's priority ----
    {
        check(choosePtViewTag(false, false, false) == PtViewTag::None,
              "nothing unavailable, nothing suppressed, nothing active: no tag");
        check(choosePtViewTag(false, false, true) == PtViewTag::Active,
              "only active: the green [active] tag");
        check(choosePtViewTag(false, true, false) == PtViewTag::SuppressedByRayDriven,
              "only suppressed: the new suppressed-by-ray-driven tag");
        check(choosePtViewTag(true, false, false) == PtViewTag::Unavailable,
              "only unavailable: the red [unavailable on this device] tag");
    }
    {
        // UNAVAILABLE OUTRANKS SUPPRESSED. PathTracer::init() can refuse a device for a reason
        // ray-driven painting has nothing to do with (a DXC compile failure, say) -- turning
        // ray-driven off would not bring it back, so "unavailable" is the honest thing to say even
        // while ray-driven also happens to be painting.
        check(choosePtViewTag(true, true, false) == PtViewTag::Unavailable,
              "unavailable AND suppressed: unavailable wins (turning ray-driven off would not help)");
        check(choosePtViewTag(true, true, true) == PtViewTag::Unavailable,
              "all three at once (should not occur, but is not undefined): unavailable still wins");
    }
    {
        // SUPPRESSED OUTRANKS ACTIVE, though by construction (syncPtSceneView() resets ptSceneView_
        // to null the instant it forces the want-flag false) the two should never both be true --
        // pinning the priority anyway documents the invariant rather than leaving it implicit.
        check(choosePtViewTag(false, true, true) == PtViewTag::SuppressedByRayDriven,
              "suppressed AND active (should not occur): suppressed wins, matching priority order");
    }

    std::printf("[INFO ] === %d assertions, %d failed ===\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
