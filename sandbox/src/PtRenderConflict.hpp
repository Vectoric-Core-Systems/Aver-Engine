// Which tag the Path Tracing page's Quality combo shows.
//
// Path Tracing on takes the viewport (SandboxApp::ptTakesViewport forces raster primary under it),
// so ray-driven only holds the path-traced view down while a ray-hit debug view needs it; that is
// the SuppressedByRayDriven case below.
//
// A pure function so tests/editor/src/PtRenderConflictTest.cpp can check it headless: plain values
// in and out, no ImGui, no SandboxApp state.
#pragma once
#include "aver/core/Types.hpp"

namespace aver::editor {

// UNAVAILABLE OUTRANKS SUPPRESSED: PathTracer::init() can refuse a device (e.g. a DXC compile
// failure), and leaving the debug view would not bring the tracer back. SUPPRESSED and ACTIVE cannot
// both be true by construction (syncPtSceneView() drops the view when it withdraws the want), but the
// priority is named here rather than left to call order.
enum class PtViewTag { None, Unavailable, SuppressedByRayDriven, Active };

inline PtViewTag choosePtViewTag(bool unavailable, bool suppressedByRayDriven, bool active) {
    if (unavailable) return PtViewTag::Unavailable;
    if (suppressedByRayDriven) return PtViewTag::SuppressedByRayDriven;
    if (active) return PtViewTag::Active;
    return PtViewTag::None;
}

} // namespace aver::editor
