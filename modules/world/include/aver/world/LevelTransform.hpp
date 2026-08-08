#pragma once
// The .ocworld rotation encoding: Euler degrees <-> quaternion.
//
// THIS IS A FILE-FORMAT CONTRACT, NOT AN EDITOR CONVENTION, which is why it lives here and not in
// the editor. It was in two places at once -- sandbox/src/EditorEuler.hpp and a file-static copy in
// modules/runtime.game/src/GameLevel.cpp -- because the game genuinely needs it to read a level the
// editor wrote. That duplication was the evidence that it belongs in the engine; both copies now
// resolve here, and EditorEuler.hpp is a re-export so every editor call site is unchanged.
//
// Header-only and Core-only ON PURPOSE. sandbox/src/EditorEuler.hpp declined to promote this on the
// grounds that it "would put editor UI conventions into the engine's dependency graph" -- the same
// trade tests/editor/CMakeLists.txt argues for EditorPrefs. That objection is answered rather than
// overridden: nothing here needs Formats, Scene or Physics, so tests/editor still compiles it with
// an include directory and no link, and the dependency graph does not move.
#include "aver/core/Math.hpp"

#include <cmath>

namespace aver::world {

// Builds a quaternion from (roll, pitch, yaw) degrees as Rz * Ry * Rx.
//
// THE MULTIPLICATION ORDER IS THE CONTRACT. A level authored by the editor is only reproduced by a
// game that composes the same way round; getting it backwards yaws things that should roll, which
// reads as bad authoring rather than as a bug in the loader.
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
// WHY IT MATTERED MORE THAN A DISPLAY GLITCH: the editor's level save reads rotations back through
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

} // namespace aver::world
