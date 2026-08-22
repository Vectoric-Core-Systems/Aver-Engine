// The one place Aver's coordinate contract is translated into Jolt's, and back.
// Aver: centimetres, +X forward, +Y right, +Z up, LEFT-handed. Jolt: metres, -Z forward, +X right,
// +Y up, RIGHT-handed.
#pragma once
#include "aver/core/Math.hpp"

#include <Jolt/Jolt.h>
#include <Jolt/Math/Quat.h>
#include <Jolt/Math/Mat44.h>
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

// --- Matrices -------------------------------------------------------------------------------------

// An engine transform to Jolt's. Needed because Jolt's soft-body skinning takes a JOINT PALETTE,
// and Aver's anim::poseToSkinning produces one in engine space.
//
// THE DERIVATION, because a wrong sign here is a mesh that skins inside out and nothing says so:
//
//   Aver's Mat4 is ROW-VECTOR (v' = v*M), so as a column-vector map its linear part is the
//   TRANSPOSE of the upper 3x3 -- which means column k of that map is literally ROW k of the Mat4.
//   Jolt's Mat44 is column-vector. Writing B for the basis change (jolt = B*aver, what toJoltUnit
//   does), the equivalent Jolt linear map is B * A * B^-1. Expanding B^-1's columns gives
//
//       col0 = B * (A's col 1) ,  col1 = B * (A's col 2) ,  col2 = -B * (A's col 0)
//
//   and A's col k is Aver's row k, so each column below is toJoltUnit of a row. Translation is a
//   POSITION and goes through toJolt, which also converts centimetres to metres -- the rotation
//   columns must NOT be scaled, which is why they use toJoltUnit and only the last uses toJolt.
//
// Checked against transformPoint in tests/physics rather than trusted: the test transforms a point
// in engine space, converts the result, and compares it with converting first and transforming in
// Jolt space. Those two agree only if every line here is right.
inline JPH::Mat44 toJolt(const Mat4& m) {
    const Vec3 row0(m.m[0][0], m.m[0][1], m.m[0][2]);
    const Vec3 row1(m.m[1][0], m.m[1][1], m.m[1][2]);
    const Vec3 row2(m.m[2][0], m.m[2][1], m.m[2][2]);
    const Vec3 row3(m.m[3][0], m.m[3][1], m.m[3][2]);
    const JPH::Vec3 c0 = toJoltUnit(row1);
    const JPH::Vec3 c1 = toJoltUnit(row2);
    const JPH::Vec3 c2 = -toJoltUnit(row0);
    const JPH::Vec3 c3 = toJolt(row3);
    return JPH::Mat44(JPH::Vec4(c0.GetX(), c0.GetY(), c0.GetZ(), 0.0f),
                      JPH::Vec4(c1.GetX(), c1.GetY(), c1.GetZ(), 0.0f),
                      JPH::Vec4(c2.GetX(), c2.GetY(), c2.GetZ(), 0.0f),
                      JPH::Vec4(c3.GetX(), c3.GetY(), c3.GetZ(), 1.0f));
}

// --- Scalars --------------------------------------------------------------------------------------

// Centimetres to metres.
inline constexpr f32 cmToM(f32 cm) { return cm / kCmPerMetre; }
// Metres to centimetres.
inline constexpr f32 mToCm(f32 m)  { return m * kCmPerMetre; }

} // namespace aver::physics
