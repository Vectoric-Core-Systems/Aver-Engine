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

// --- Axial (pseudo)vectors: angular velocity, torque, angular impulse ------------------------------
//
// THESE FLIP SIGN, AND A LINEAR CONVERTER USED HERE IS WRONG IN A WAY NOTHING REPORTS. The basis map
// above sends aver (x,y,z) to jolt (y,z,-x), i.e.
//
//     B = [ 0  1  0 ]
//         [ 0  0  1 ]      det(B) = -1
//         [-1  0  0 ]
//
// and det(B) = -1 is the whole of the left-handed to right-handed flip. A POLAR vector (a position, a
// direction, a velocity, a force, a linear impulse) transforms by B. An AXIAL vector -- one defined by
// a cross product, which is every rotational quantity: angular velocity, torque, angular momentum --
// transforms by det(B) * B, so it picks up the extra minus sign.
//
// The evidence that this is not a derivation-on-paper: toJolt(Quat) above is (-q.y, -q.z, q.x, q.w),
// whose vector part is exactly the NEGATIVE of toJoltUnit's (v.y, v.z, -v.x). A quaternion's vector
// part is its rotation axis times sin(theta/2) -- an axial vector -- so the rotation converter this
// module has shipped and tested all along already carries this sign. These helpers only give the same
// rule a name, so a caller reaching for "the direction one" cannot silently get a body spinning
// backwards.
//
// UNSCALED, like toJoltUnit: angular velocity is radians per second and radians are dimensionless, so
// there is no centimetre in it to convert. Torque and angular impulse DO carry length (and length
// squared at that) and get their own converters below.

// Engine angular velocity (rad/s) to Jolt's. Axes only -- radians need no scaling.
inline JPH::Vec3 toJoltAngular(const Vec3& v)   { return JPH::Vec3(-v.y, -v.z, v.x); }
// Jolt angular velocity (rad/s) to the engine's.
inline Vec3      fromJoltAngular(JPH::Vec3Arg v) { return Vec3(v.GetZ(), -v.GetX(), -v.GetY()); }

// Engine torque (kg*cm^2/s^2) or angular impulse (kg*cm^2/s) to Jolt's SI equivalent.
//
// TWO FACTORS OF kCmPerMetre, not one, and that is the difference between a torque and a force. A
// torque is a force times a LEVER ARM: both the force's own length dimension and the arm's have to be
// converted, so the scale is cm^2 -> m^2. Getting this wrong by one factor makes every applied torque
// a hundred times too weak, which reads as "torque does not work" rather than as a units bug.
inline JPH::Vec3 toJoltTorque(const Vec3& v) {
    return toJoltAngular(v) / (kCmPerMetre * kCmPerMetre);
}

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
