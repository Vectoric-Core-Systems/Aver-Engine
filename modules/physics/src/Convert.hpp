// The one place Aver's coordinate contract is translated into Jolt's, and back.
// Aver: centimetres, +X forward, +Y right, +Z up, LEFT-handed. Jolt: metres, -Z forward, +X right,
// +Y up, RIGHT-handed.
#pragma once
#include "aver/core/Math.hpp"

#include <Jolt/Jolt.h>
#include <Jolt/Math/Quat.h>
#include <Jolt/Math/Vec3.h>

namespace aver::physics {

inline constexpr f32 kCmPerMetre = 100.0f;

// --- Positions and distances (scaled) -------------------------------------------------------------

// Engine position in centimetres to a Jolt position in metres.
inline JPH::Vec3 toJolt(const Vec3& v) {
    return JPH::Vec3(v.y / kCmPerMetre, v.z / kCmPerMetre, -v.x / kCmPerMetre);
}

// Jolt position in metres to an engine position in centimetres.
inline Vec3 fromJolt(JPH::Vec3Arg v) {
    return Vec3(-v.GetZ() * kCmPerMetre, v.GetX() * kCmPerMetre, v.GetY() * kCmPerMetre);
}

// --- Directions and velocities --------------------------------------------------------------------

// Engine direction or velocity (cm, cm/s) to Jolt's (m, m/s).
inline JPH::Vec3 toJoltDir(const Vec3& v)   { return toJolt(v); }
// Jolt direction or velocity (m, m/s) to the engine's (cm, cm/s).
inline Vec3      fromJoltDir(JPH::Vec3Arg v) { return fromJolt(v); }

// Engine unit vector to Jolt's axes, UNSCALED.
inline JPH::Vec3 toJoltUnit(const Vec3& v) { return JPH::Vec3(v.y, v.z, -v.x); }
// Jolt unit vector to the engine's axes, UNSCALED.
inline Vec3      fromJoltUnit(JPH::Vec3Arg v) { return Vec3(-v.GetZ(), v.GetX(), v.GetY()); }

// --- Rotations ------------------------------------------------------------------------------------

// Engine rotation to Jolt's: vector part permuted and negated, w untouched.
inline JPH::Quat toJolt(const Quat& q) {
    return JPH::Quat(-q.y, -q.z, q.x, q.w);
}

// Jolt rotation to the engine's.
inline Quat fromJolt(JPH::QuatArg q) {
    return Quat(q.GetZ(), -q.GetX(), -q.GetY(), q.GetW());
}

// --- Scalars --------------------------------------------------------------------------------------

// Centimetres to metres.
inline constexpr f32 cmToM(f32 cm) { return cm / kCmPerMetre; }
// Metres to centimetres.
inline constexpr f32 mToCm(f32 m)  { return m * kCmPerMetre; }

} // namespace aver::physics
