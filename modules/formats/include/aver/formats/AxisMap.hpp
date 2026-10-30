#pragma once
// The one basis change from a right-handed, +Y up, metre source (glTF and other importers) to engine space (left-handed, +Z up,
// +X forward, centimetres). GltfImport.cpp's toEngine / toEngineQuat / toEngineScale call these, and so does every scene
// importer, so a model and a scene land in the same place.
#include "aver/core/Math.hpp"
#include "aver/core/Types.hpp"

namespace aver::fmt::axis {

inline constexpr f32 kCmPerMetre = 100.0f;

// A direction or any unitless vector: (x, y, z) -> (-z, x, y).
inline Vec3 direction(f32 x, f32 y, f32 z) { return Vec3{-z, x, y}; }

// A position in metres -> centimetres.
inline Vec3 position(f32 x, f32 y, f32 z) { return Vec3{-z * kCmPerMetre, x * kCmPerMetre, y * kCmPerMetre}; }

// The map has determinant -1, so a rotation's axis is a pseudovector: the vector part takes a sign, w does not.
inline Quat rotation(f32 x, f32 y, f32 z, f32 w) { return Quat{z, -x, -y, w}; }

// Scale is dimensionless: it permutes with the axes only.
inline Vec3 scale(f32 x, f32 y, f32 z) { return Vec3{z, x, y}; }

}  // namespace aver::fmt::axis
