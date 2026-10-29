// Local fog volumes (G4 R8, docs/plans/fabric/G4_RENDER.md): Godot's FogVolume + FogMaterial as shaped regions of extra extinction, scattering and
// emission inside Voxi's volumetric-fog froxel volume. Header-only and pure: the framework registry, the renderer and the tests share one set of formulas,
// and the HLSL in voxi_froxel.hlsl (AVER_FOG_VOLUMES) mirrors fogVolumeWeight() line by line.
//
// Engine space: centimetres, +Z up, left-handed, row vectors. A volume is a unit box [-1,1]^3 carried by an affine matrix; the shape lives in that unit
// space, so one world->unit transform (three float4 rows) is all the shader needs. Godot's local Y axis is the engine's local Z (the axis map sends
// Godot (x, y, z) to engine (-z, x, y)), so "up" for the cone and the cylinder is local +Z here.
#pragma once
#include "aver/core/Types.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace aver {

// Godot's RenderingServer.FogVolumeShape numbers.
enum class FogVolumeShape : i32 { Ellipsoid = 0, Cone = 1, Cylinder = 2, Box = 3, World = 4 };

inline constexpr u32 kFogVolumeMax = 16;       // volumes in one frame's block (mirrors AVER_FV_MAX in voxi_froxel.hlsl)
inline constexpr u32 kFogVolumeVecs = 6;       // float4 per volume (mirrors AVER_FV_VECS)

// One volume as the host knows it. Lengths in centimetres, colours linear.
struct FogVolumeDesc {
    FogVolumeShape shape = FogVolumeShape::Box;
    // The engine instance matrix: three basis rows then the translation (12 floats, row vectors). The basis carries the node's scale.
    f32 world[12] = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};
    // Half size along the engine's local X, Y, Z before the matrix's scale (Godot size / 2, axis-mapped, centimetres).
    f32 halfExtentCm[3] = {100, 100, 100};
    f32 densityPerM = 1.0f;       // FogMaterial.density: extinction per metre; may be negative (it carves)
    f32 albedo[3] = {1, 1, 1};    // scattering colour, linear
    f32 emission[3] = {0, 0, 0};  // radiance added per unit density, linear
    f32 heightFalloff = 0.0f;     // per metre; 0 = uniform
    f32 edgeFade = 0.1f;          // depth of the soft edge, as a fraction of the half size
    // G6 SH3: a converted Godot fog shader (1-based index into the frame's fog-shader table, 0 = none). The shader replaces density, albedo and emission
    // (density is per metre, as FogMaterial.density is); the shape and the edge fade still apply, the height falloff does not.
    u32 shader = 0;
};

// The shape field of a packed volume carries the shape in its low three bits and the fog-shader index above them (0 = no shader, today's volumes).
inline constexpr i32 kFogVolumeShapeBits = 3;
inline i32 fogVolumeShapeOf(f32 packed) { return static_cast<i32>(packed + 0.5f) & ((1 << kFogVolumeShapeBits) - 1); }
inline u32 fogVolumeShaderOf(f32 packed) { return static_cast<u32>(static_cast<i32>(packed + 0.5f) >> kFogVolumeShapeBits); }

// What the shader reads for one volume (kFogVolumeVecs float4s):
//   toUnit0..2  world position -> unit-box coordinates: u_i = dot(p, rowi.xyz) + rowi.w
//   params      x extinction per cm, y edge fade, z height falloff per cm, w shape
//   albedo      rgb albedo, w the world Z of the volume's lowest point (the height falloff starts there), cm
//   emission    rgb emission, w smallest half size in cm (so a falloff band is metric)
struct FogVolumeBlock {
    f32 header[4] = {};                          // x = volume count
    f32 vec[kFogVolumeMax][kFogVolumeVecs][4] = {};
};
static_assert(sizeof(FogVolumeBlock) == 16 + kFogVolumeMax * kFogVolumeVecs * 16, "the HLSL tail mirrors this byte for byte");

namespace fogv {

// Inverse of the affine map p' = p * B + t (B = rows 0..2, t = row 3), returned as the three rows of p = ... that take a WORLD point to LOCAL
// coordinates: out_i = dot(p, col_i) + off_i. False when the basis is singular (a zero scale).
inline bool invertAffine(const f32 w[12], f32 rows[3][4]) {
    const f32 a = w[0], b = w[1], c = w[2], d = w[3], e = w[4], f = w[5], g = w[6], h = w[7], i = w[8];
    const f32 det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
    if (!(std::fabs(det) > 1e-20f)) return false;
    const f32 inv = 1.0f / det;
    // Inverse of the 3x3 (row-vector convention: local = (p - t) * B^-1).
    const f32 m[3][3] = {{(e * i - f * h) * inv, (c * h - b * i) * inv, (b * f - c * e) * inv},
                         {(f * g - d * i) * inv, (a * i - c * g) * inv, (c * d - a * f) * inv},
                         {(d * h - e * g) * inv, (b * g - a * h) * inv, (a * e - b * d) * inv}};
    const f32 t[3] = {w[9], w[10], w[11]};
    for (int j = 0; j < 3; ++j) {
        rows[j][0] = m[0][j]; rows[j][1] = m[1][j]; rows[j][2] = m[2][j];
        rows[j][3] = -(t[0] * m[0][j] + t[1] * m[1][j] + t[2] * m[2][j]);
    }
    return true;
}

// The lowest world Z of the unit box carried by `w` with half sizes `half` (cm): the translation's Z minus the extent of the three scaled axes.
inline f32 lowestZ(const f32 w[12], const f32 half[3]) {
    return w[11] - (std::fabs(w[2]) * half[0] + std::fabs(w[5]) * half[1] + std::fabs(w[8]) * half[2]);
}

// Packs one volume. False (and `out` untouched) when the matrix cannot be inverted or the density is not finite.
inline bool pack(const FogVolumeDesc& v, f32 out[kFogVolumeVecs][4]) {
    if (!std::isfinite(v.densityPerM)) return false;
    f32 inv[3][4];
    if (!invertAffine(v.world, inv)) return false;
    // local (engine units) -> unit box: divide by the half size.
    for (int j = 0; j < 3; ++j) {
        const f32 h = std::max(v.halfExtentCm[j], 1e-3f);
        for (int k = 0; k < 4; ++k) out[j][k] = inv[j][k] / h;
    }
    const f32 minHalf = std::max(std::min({v.halfExtentCm[0], v.halfExtentCm[1], v.halfExtentCm[2]}), 1e-3f);
    out[3][0] = v.densityPerM * 0.01f;
    out[3][1] = std::max(v.edgeFade, 1e-3f);
    out[3][2] = std::max(v.heightFalloff, 0.0f) * 0.01f;
    out[3][3] = static_cast<f32>(static_cast<i32>(v.shape) + (static_cast<i32>(v.shader) << kFogVolumeShapeBits));
    out[4][0] = std::clamp(v.albedo[0], 0.0f, 1.0f); out[4][1] = std::clamp(v.albedo[1], 0.0f, 1.0f); out[4][2] = std::clamp(v.albedo[2], 0.0f, 1.0f);
    out[4][3] = lowestZ(v.world, v.halfExtentCm);
    out[5][0] = std::max(v.emission[0], 0.0f); out[5][1] = std::max(v.emission[1], 0.0f); out[5][2] = std::max(v.emission[2], 0.0f);
    out[5][3] = minHalf;
    if (v.shader != 0) {
        // The shader supplies albedo and emission, so those slots carry what it needs instead: the volume's centre (world cm) and its local half extents (cm).
        out[4][0] = v.world[9]; out[4][1] = v.world[10]; out[4][2] = v.world[11];
        out[5][0] = v.halfExtentCm[0]; out[5][1] = v.halfExtentCm[1]; out[5][2] = v.halfExtentCm[2];
    }
    return true;
}

// Packs up to kFogVolumeMax volumes; the ones that cannot be packed are skipped. Returns how many were packed.
inline u32 packAll(const FogVolumeDesc* v, u32 count, FogVolumeBlock& block) {
    block = FogVolumeBlock{};
    u32 n = 0;
    for (u32 i = 0; i < count && n < kFogVolumeMax; ++i)
        if (pack(v[i], block.vec[n])) ++n;
    block.header[0] = static_cast<f32>(n);
    return n;
}

// Signed distance to the shape's surface in unit-box coordinates (negative inside, in units of the half size; not Euclidean for a stretched volume).
// World has no surface: -1 everywhere. Cone: tip at local +Z, base (full radius) at local -Z. Cylinder: axis local Z.
inline f32 sdf(FogVolumeShape shape, const f32 u[3]) {
    switch (shape) {
        case FogVolumeShape::Box:
            return std::max({std::fabs(u[0]), std::fabs(u[1]), std::fabs(u[2])}) - 1.0f;
        case FogVolumeShape::Ellipsoid:
            return std::sqrt(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]) - 1.0f;
        case FogVolumeShape::Cylinder:
            return std::max(std::sqrt(u[0] * u[0] + u[1] * u[1]) - 1.0f, std::fabs(u[2]) - 1.0f);
        case FogVolumeShape::Cone: {
            // Radius shrinks linearly from 1 at u.z = -1 to 0 at u.z = +1.
            const f32 r = std::sqrt(u[0] * u[0] + u[1] * u[1]);
            const f32 side = r - 0.5f * (1.0f - u[2]);
            return std::max(side, std::fabs(u[2]) - 1.0f);
        }
        case FogVolumeShape::World:
        default:
            return -1.0f;
    }
}

// The volume's signed density at a world point: density * edge fade * height falloff. This is the number the shader adds to the medium's extinction
// (per cm); it can be negative. `p` is the world position in cm.
inline f32 density(const f32 pk[kFogVolumeVecs][4], const f32 p[3]) {
    f32 u[3];
    for (int j = 0; j < 3; ++j) u[j] = p[0] * pk[j][0] + p[1] * pk[j][1] + p[2] * pk[j][2] + pk[j][3];
    const FogVolumeShape shape = static_cast<FogVolumeShape>(fogVolumeShapeOf(pk[3][3]));
    const f32 d = sdf(shape, u);
    if (d >= 0.0f) return 0.0f;
    f32 weight = 1.0f;
    if (shape != FogVolumeShape::World) weight = std::clamp(-d / pk[3][1], 0.0f, 1.0f);
    f32 height = 1.0f;
    if (pk[3][2] > 0.0f) height = std::exp(-pk[3][2] * std::max(p[2] - pk[4][3], 0.0f));
    return pk[3][0] * weight * height;
}

} // namespace fogv
} // namespace aver
