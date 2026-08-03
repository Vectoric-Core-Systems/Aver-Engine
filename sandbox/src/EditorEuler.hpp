#pragma once
// The editor's Euler <-> quaternion pair, extracted so it can be TESTED.
//
// It lived as two static functions inside SandboxApp.cpp, unreachable from any test, and one of them
// was wrong for eight months in a way that corrupted saved levels. See eulerDegFromQuat.
//
// Header-only and still editor-local: promoting this to a module would put editor UI conventions
// into the engine's dependency graph, which is the trade tests/editor/CMakeLists.txt already argues
// against for EditorPrefs. A test compiles this header directly instead.
#include "aver/core/Math.hpp"

#include <cmath>

namespace aver::editor {

// Builds a quaternion from (roll, pitch, yaw) degrees as Rz * Ry * Rx.
//
// THE MULTIPLICATION ORDER IS THE CONTRACT. modules/runtime.game/src/GameLevel.cpp:30 composes the
// same way round, and a level authored by the editor is only reproduced by a game that agrees.
inline Quat quatFromEulerDeg(const Vec3& e) {
    return (Quat::fromAxisAngle({0,0,1}, radians(e.z)) * Quat::fromAxisAngle({0,1,0}, radians(e.y)) *
            Quat::fromAxisAngle({1,0,0}, radians(e.x))).normalized();
}

// Exact inverse of quatFromEulerDeg. Returns (roll, pitch, yaw) degrees.
//
// THE GIMBAL BRANCH USED THE WRONG DENOMINATOR, and this is the fix. It divided by
// 1 - 2(y*y + z*z), which for this ZYX composition is the rotated X axis's x component --
// cos(pitch) * cos(yaw) -- and that is identically zero exactly when |sin(pitch)| ~ 1, which IS the
// branch condition. atan2 with a ~0 denominator returns whatever the float residue's sign says:
// +-90 degrees, or +-180 when the numerator is also zero.
//
// Measured before the fix, with the composed quaternion's |dot| against the original:
//     (0, 90, 30)  read back (0, 90,  90)   |dot| 0.866   -- 60 degrees wrong
//     (0, 90,  0)  read back (0, 90, -180)  |dot| 0.000   -- a full 180 degree flip
//     (0,-90, 45)  read back (0,-90,  90)   |dot| 0.924
//
// The correct denominator is the rotated Y axis's y component, 1 - 2(x*x + z*z) = cos(yaw - roll),
// which pairs with the existing numerator -2(x*y - w*z) = sin(yaw - roll) to give atan2 -> yaw - roll.
// Roll is pinned to 0 in this branch -- at gimbal lock only their difference is observable -- so the
// result is the yaw.
//
// WHY IT MATTERED MORE THAN A DISPLAY GLITCH: SandboxApp's level save reads rotations back through
// this function and writes them into the .ocworld, so any entity within 0.26 degrees of vertical was
// PERSISTED wrong. Save and reload silently reoriented it. The rotate gizmo, the move and scale
// gizmos, undo/redo and the transform panel all round-trip through here as well.
inline Vec3 eulerDegFromQuat(const Quat& q) {
    const f32 sinP = 2.0f * (q.w * q.y - q.z * q.x);
    const f32 pitch = std::asin(std::fmax(-1.0f, std::fmin(1.0f, sinP)));
    f32 roll, yaw;
    if (std::fabs(sinP) > 0.99999f) {
        // Gimbal lock: roll and yaw are no longer separable, only their difference is. Pin roll and
        // put the whole rotation about the vertical into yaw.
        roll = 0.0f;
        yaw  = std::atan2(-2.0f * (q.x * q.y - q.w * q.z), 1.0f - 2.0f * (q.x * q.x + q.z * q.z));
    } else {
        roll = std::atan2(2.0f * (q.w * q.x + q.y * q.z), 1.0f - 2.0f * (q.x * q.x + q.y * q.y));
        yaw  = std::atan2(2.0f * (q.w * q.z + q.x * q.y), 1.0f - 2.0f * (q.y * q.y + q.z * q.z));
    }
    const f32 r2d = 180.0f / 3.14159265358979323846f;
    return Vec3{roll * r2d, pitch * r2d, yaw * r2d};
}

} // namespace aver::editor
