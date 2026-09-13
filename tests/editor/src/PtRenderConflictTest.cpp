// PtRenderConflictTest -- the two pure decisions behind A1/A2 of the ray-driven/Path-Tracing
// suppression work: which tag the Path Tracing page's Quality combo shows (PtRenderConflict.hpp's
// choosePtViewTag), and whether a project's EFFECTIVE render settings self-contradict by asking for
// both ray-driven primary visibility and Path Tracing at once (checkPtRtConflict).
//
// Header-only and dependency-free, for InputOwnershipTest.cpp's exact reason (see that file's own
// top comment): no ImGui, no SandboxApp, no AVER_WARN, no voxi::Renderer -- PtRenderConflict.hpp
// takes plain bools/u32 in and returns plain data out, so the two decisions SandboxApp.cpp's
// buildUI()/applyProjectRenderSettings() make are reachable without a window, a device, or a project
// file. This is COMPILED, not run, by this lane's own build -- see the task's own verification rule.
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

    // ---- A2: checkPtRtConflict on the effective settings ----
    {
        // THE ORDINARY CASE: ray-driven mode with Path Tracing off. No manifest ships this pair by
        // accident often, but it is the shipped DEFAULT (rtRenderMode's own default is 1, pathTracing
        // defaults to Off) and must never warn.
        const PtRtConflict c = checkPtRtConflict(1u, false, false, false);
        check(!c.conflicts, "rtRenderMode 1, Path Tracing Off: no conflict (the shipped default)");
    }
    {
        // Rasteriser mode with Path Tracing on: also fine, this is precisely how a project SEES its
        // path-traced view (per this file's own SandboxApp.cpp comment: "--rt-render-mode 0 the path
        // tracer DOES paint").
        const PtRtConflict c = checkPtRtConflict(0u, true, false, false);
        check(!c.conflicts, "rtRenderMode 0, Path Tracing on: no conflict (this is how PT is seen)");
    }
    {
        // THE DIAGNOSIS'S OWN CASE: PTTest.ocproject's RENDER.RTRENDERMODE 1 + RENDER.PATHTRACING 4,
        // with no command line involved at all.
        const PtRtConflict c = checkPtRtConflict(1u, true, false, false);
        check(c.conflicts, "rtRenderMode 1 AND Path Tracing on, no CLI: conflicts");
        check(!c.decidedByCli, "...and it is the manifest alone that says so");
    }
    {
        // A CLI FLAG CHANGED THE OUTCOME: --rt-render-mode moved the effective value away from what
        // the manifest alone produced, and Path Tracing is (still) on -- the command line is what put
        // the scene in this state, and the warning should credit it by name.
        const PtRtConflict c = checkPtRtConflict(1u, true, /*rtRenderModeChangedByCli=*/true, false);
        check(c.conflicts, "rtRenderMode 1 AND Path Tracing on, CLI moved rtRenderMode: conflicts");
        check(c.decidedByCli, "...credited to the command line, since it is what produced this value");
    }
    {
        // THE SAME, but via --pt instead of --rt-render-mode: a flag that turned Path Tracing ON over
        // a manifest that had it Off, against a manifest/default rtRenderMode of 1.
        const PtRtConflict c = checkPtRtConflict(1u, true, false, /*pathTracingOnChangedByCli=*/true);
        check(c.conflicts, "rtRenderMode 1 (manifest), Path Tracing turned on by --pt: conflicts");
        check(c.decidedByCli, "...credited to the command line for the same reason");
    }
    {
        // A REDUNDANT FLAG MUST NOT BE CREDITED. --rt-render-mode 1 given on the command line against
        // a manifest that ALREADY said 1 changes nothing -- the effective value is identical to what
        // the manifest alone would have produced, so this must read as "the manifest says this", not
        // "the command line decided it". This is the case the naive "was a flag given at all" version
        // of this check would have gotten wrong.
        const PtRtConflict c = checkPtRtConflict(1u, true, /*rtRenderModeChangedByCli=*/false, false);
        check(c.conflicts, "rtRenderMode 1 AND Path Tracing on, flag given but redundant: still conflicts");
        check(!c.decidedByCli, "...but NOT credited to the command line, since nothing actually moved");
    }
    {
        // A FLAG THAT RESOLVES THE CONFLICT LEAVES NOTHING TO WARN ABOUT. This is exercised at the
        // call site by feeding checkPtRtConflict the EFFECTIVE (post-override) settings in the first
        // place -- once rtRenderMode's effective value is 0, there is no conflict left to name, CLI
        // or otherwise, which is exactly what an unconditional pass-the-effective-values call site
        // (applyProjectRenderSettings) gets for free without an explicit branch.
        const PtRtConflict c = checkPtRtConflict(0u, true, /*rtRenderModeChangedByCli=*/true, false);
        check(!c.conflicts, "CLI moved rtRenderMode to 0: no conflict left, regardless of the manifest");
    }

    std::printf("[INFO ] === %d assertions, %d failed ===\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
