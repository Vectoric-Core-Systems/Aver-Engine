#pragma once
// Grid and snap maths for the viewport options bar (editor polish step 10). ImGui-free, so
// ViewportSnapTest links nothing; SandboxApp.hpp's snapf() delegates here so there is one rounding rule.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace aver::editor {

enum class SnapKind { Move, Rotate, Scale };

// Rounds to the nearest multiple of `step`; a non-positive step means no snapping.
inline float snapValue(float v, float step) { return step > 0.0f ? std::round(v / step) * step : v; }

// The step ranges the editor preferences already allow.
inline float clampSnapStep(SnapKind k, float v) {
    switch (k) {
        case SnapKind::Move:   return std::min(std::max(v, 0.01f), 10000.0f);
        case SnapKind::Rotate: return std::min(std::max(v, 0.1f), 180.0f);
        case SnapKind::Scale:  return std::min(std::max(v, 0.01f), 10.0f);
    }
    return v;
}

inline constexpr float kMoveSnapPresets[]   = {0.1f, 0.25f, 0.5f, 1.0f, 2.0f, 5.0f, 10.0f, 50.0f, 100.0f};
inline constexpr float kRotateSnapPresets[] = {1.0f, 5.0f, 10.0f, 15.0f, 30.0f, 45.0f, 90.0f};
inline constexpr float kScaleSnapPresets[]  = {0.05f, 0.1f, 0.25f, 0.5f, 1.0f};

// "10", "0.25", "15" + the degree sign for rotation (UTF-8).
inline std::string snapLabel(SnapKind k, float v) {
    char b[32];
    std::snprintf(b, sizeof b, "%g", static_cast<double>(v));
    std::string s = b;
    if (k == SnapKind::Rotate) s += "\xC2\xB0";
    return s;
}

} // namespace aver::editor
