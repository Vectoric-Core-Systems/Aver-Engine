#pragma once
// The ONE place Aver's coordinate contract is translated into Jolt's, and back.
//
// Every one of the four conventions differs, which is why this is a named, tested file rather than a
// few casts at the call sites:
//
//              Aver                                   Jolt
//   units      centimetres                            metres
//   up         +Z                                     +Y
//   forward    +X                                     -Z
//   right      +Y                                     +X
//   handed     LEFT                                   RIGHT
//
// The units are not a relabelling. Jolt documents its accuracy as assuming dynamic bodies in
// [0.1, 10] m and gravity in [0, 10] m/s^2, and says outright: "if you are using different units,
// consider scaling the objects before passing them on to the physics simulation". Passing
// centimetres straight through would place a 160cm character at "160 m" and gravity at 980 -- both
// outside the range the library is documented to be accurate in.
//
// The axis map is the signed permutation
//
//   jolt = ( aver.y, aver.z, -aver.x )        aver = ( -jolt.z, jolt.x, jolt.y )
//
// which sends Aver's right/up/forward onto Jolt's right/up/forward. Its determinant is -1, and that
// is deliberate rather than an accident: a determinant of -1 is exactly what flips handedness, which
// is what a left-handed-to-right-handed change of basis has to do.
//
// The determinant is also why the QUATERNION map is not simply the same permutation applied to the
// vector part. Under an improper change of basis M (det -1), an axis-angle rotation (n, theta) maps
// to (M n, -theta) -- the rotation SENSE reverses. In quaternion terms the vector part is permuted
// and negated while w is left alone. Getting this wrong yields rotations that look plausible and are
// mirrored, so PhysicsTest checks the defining property directly:
//
//   toJolt(rotate(q, v))  ==  rotate(toJolt(q), toJolt(v))
#include "aver/core/Math.hpp"

#include <Jolt/Jolt.h>
#include <Jolt/Math/Quat.h>
#include <Jolt/Math/Vec3.h>

namespace aver::physics {

// Centimetres per metre. Named because "100" appearing bare in a conversion is the kind of constant
// that gets copied to a call site and then not updated.
inline constexpr f32 kCmPerMetre = 100.0f;

// --- Positions and distances (scaled) -------------------------------------------------------------

inline JPH::Vec3 toJolt(const Vec3& v) {
    return JPH::Vec3(v.y / kCmPerMetre, v.z / kCmPerMetre, -v.x / kCmPerMetre);
}

inline Vec3 fromJolt(JPH::Vec3Arg v) {
    return Vec3(-v.GetZ() * kCmPerMetre, v.GetX() * kCmPerMetre, v.GetY() * kCmPerMetre);
}

// --- Directions and velocities --------------------------------------------------------------------
// Same axis map, but velocities are cm/s vs m/s, so they scale exactly like positions. Separate names
// only so a call site reads as what it is; a direction converted with the position function is not
// wrong, it is just harder to review.
inline JPH::Vec3 toJoltDir(const Vec3& v)   { return toJolt(v); }
inline Vec3      fromJoltDir(JPH::Vec3Arg v) { return fromJolt(v); }

// A pure direction that must NOT be scaled -- an already-normalised axis, where dividing by 100 and
// renormalising is just lost precision.
inline JPH::Vec3 toJoltUnit(const Vec3& v) { return JPH::Vec3(v.y, v.z, -v.x); }
inline Vec3      fromJoltUnit(JPH::Vec3Arg v) { return Vec3(-v.GetZ(), v.GetX(), v.GetY()); }

// --- Rotations ------------------------------------------------------------------------------------
// Vector part permuted AND negated, w untouched: see the note above on why the sense reverses.
inline JPH::Quat toJolt(const Quat& q) {
    return JPH::Quat(-q.y, -q.z, q.x, q.w);
}

inline Quat fromJolt(JPH::QuatArg q) {
    return Quat(q.GetZ(), -q.GetX(), -q.GetY(), q.GetW());
}

// --- Scalars --------------------------------------------------------------------------------------
inline constexpr f32 cmToM(f32 cm) { return cm / kCmPerMetre; }
inline constexpr f32 mToCm(f32 m)  { return m * kCmPerMetre; }

} // namespace aver::physics
