// WHO IS ACTUALLY PAINTING THE SCENE, WHEN A PROJECT ASKS FOR TWO THINGS THAT CANNOT BOTH RUN.
//
// THE DIAGNOSIS THIS HEADER SERVES. PTTest.ocproject records both RENDER.RTRENDERMODE 1 (ray-driven
// primary visibility) and RENDER.PATHTRACING 4 (Path Tracing, Epic). Only one render feature can ever
// win the D3D12Device.cpp/VulkanDevice.cpp beginFrame() election (the first registered IRenderFeature
// whose suppressesScene() is true), and Voxi -- ray-driven -- always registers before PtSceneView ever
// can (see SandboxApp.cpp's syncPtSceneView()), so ray-driven always wins whenever both want the
// frame. Until now nothing said so anywhere a human would see it: the Path Tracing page's Quality
// combo just silently did nothing, and a project manifest stating both settings looked no different
// from one stating a sensible pair.
//
// A PURE FUNCTION, DELIBERATELY, for InputOwnership.hpp's exact reason (see that header's own top
// comment): no ImGui types, no SandboxApp state, no AVER_WARN call, no globals -- plain values in,
// plain values out. That is what makes the two decisions below headless unit tests
// (tests/editor/src/PtRenderConflictTest.cpp), in a codebase where almost nothing about the editor
// can be tested at all. Every ImGui call and every AVER_WARN stays at the call site; only the
// DECISION moves here.
#pragma once
#include "aver/core/Types.hpp"

namespace aver::editor {

// A1 -- THE PATH TRACING PAGE'S QUALITY-COMBO TAG. Mirrors the priority the existing
// ptSceneViewUnavailable_ check already applies (SandboxApp.cpp, page == 4's buildUI() block):
//
// UNAVAILABLE OUTRANKS SUPPRESSED. A project can be both at once (PathTracer::init() already
// refused this session for a runtime reason, e.g. a DXC compile failure, AND ray-driven happens to
// be painting too) -- and "unavailable" is the more useful thing to say, because switching ray-driven
// off would NOT bring the path tracer back on this device. Telling the user "set Ray Tracing to
// Rasteriser" when that would still leave them looking at a red [unavailable] tag would be a second,
// worse silence dressed up as an answer.
//
// SUPPRESSED and ACTIVE cannot both be true by construction (syncPtSceneView() resets ptSceneView_ to
// null the moment it forces ptSceneViewWantEnabled_ false -- see that function's own top comment), but
// the priority is still named here rather than left to be reverse-engineered from call order.
enum class PtViewTag { None, Unavailable, SuppressedByRayDriven, Active };

inline PtViewTag choosePtViewTag(bool unavailable, bool suppressedByRayDriven, bool active) {
    if (unavailable) return PtViewTag::Unavailable;
    if (suppressedByRayDriven) return PtViewTag::SuppressedByRayDriven;
    if (active) return PtViewTag::Active;
    return PtViewTag::None;
}

// A2 -- DOES THE EFFECTIVE RENDER CONFIGURATION SELF-CONTRADICT. "Effective" means AFTER
// applyProjectRenderSettings' own "COMMAND LINE OUTRANKS THE MANIFEST" block has already resolved
// flag-over-manifest precedence -- NOT the raw project_.rtRenderMode/project_.pathTracing manifest
// fields. A `--rt-render-mode 0` on the command line resolves the contradiction before this ever
// runs; checking the raw manifest fields instead would warn about a conflict a human at the keyboard
// already settled, which is the exact silent-then-wrong-message shape that block's own comments
// describe having been bitten by, repeatedly, for other knobs.
//
// decidedByCli separates "the manifest alone is self-contradictory" from "a command-line flag is
// what produced this particular outcome", so the caller can credit the human's choice by name
// instead of blaming a manifest the flag has already overruled.
struct PtRtConflict {
    bool conflicts = false;
    bool decidedByCli = false;
};

inline PtRtConflict checkPtRtConflict(u32 effectiveRtRenderMode, bool effectivePathTracingOn,
                                       bool rtRenderModeSetByCli, bool pathTracingSetByCli) {
    PtRtConflict c;
    c.conflicts = (effectiveRtRenderMode == 1u) && effectivePathTracingOn;
    c.decidedByCli = c.conflicts && (rtRenderModeSetByCli || pathTracingSetByCli);
    return c;
}

} // namespace aver::editor
