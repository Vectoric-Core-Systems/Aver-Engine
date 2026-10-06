#pragma once
// Scene lights as the renderer sees them: the host-facing SceneLight, the 80-byte GPU record, the
// packing between them, and a CPU mirror of the rectangle maths in render.pt/shaders/aver_lights.hlsli
// (used by tests; the shader is the one that runs). Depends on Aver.Core only.
// See docs/rendering/LIGHTS.md for units, the rectangle model and what is approximated.
#include "aver/core/Math.hpp"
#include "aver/core/Types.hpp"

#include <algorithm>
#include <cmath>

namespace aver::voxi {

// A light in world space, filled by the host from scene::gatherLights.
struct SceneLight {
    i32  kind = 0;                        // scene::kLightPoint (0) / kLightSpot (1) / kLightRect (3)
    f32  pos[3]   = {0, 0, 0};            // cm
    f32  axis[3]  = {1, 0, 0};            // unit emission axis
    f32  right[3] = {0, 1, 0};            // unit tangent: rect width axis, IES 0 degrees
    f32  colour[3] = {1, 1, 1};
    f32  intensityCd = 0.0f;              // candela; for a rect, along its normal
    f32  rangeCm = 0.0f;                  // 0 = derived from intensity
    f32  innerCos = 1.0f, outerCos = 0.7f;   // spot cone; outerCos also frames a cookie
    f32  widthCm = 100.0f, heightCm = 100.0f;   // rect
    f32  radiusCm = 1.0f;                 // point/spot emitter radius (soft shadows)
    bool castShadows = true;
    bool iesPeak = false;                 // normalise the IES profile to its peak, not its flux
    u64  iesId = 0, cookieId = 0;         // registerIesProfile / registerCookie ids; 0 = none
};

// One light as the GPU reads it (AverLightRec). Field meanings are documented in aver_lights.hlsli.
struct PackedLight {
    f32 posRadius[4];
    f32 radianceRange[4];
    f32 axisKind[4];
    f32 shape[4];
    f32 right[4];
};
static_assert(sizeof(PackedLight) == 80, "PackedLight is the HLSL AverLightRec ABI");

inline constexpr u32 kLightKindPoint = 0, kLightKindSpot = 1, kLightKindRect = 2;
inline constexpr u32 kLightNoShadowBit = 8;

// One engine radiance unit in cd/m^2; equals rhi::kLuminanceToCdm2 (checked in VoxiRenderer.cpp).
inline constexpr f32 kEngineUnitCdm2 = 100000.0f / 3.0f;
// Auto range stops where the light is this fraction of the sun (as emissive lamps do).
inline constexpr f32 kLightRangeCutoff = 0.001f;
inline constexpr f32 kLightAutoRangeMaxCm = 5000.0f;
inline constexpr f32 kLightRangeMaxCm = 100000.0f;

// What lightAssetIndex resolved for the light's IES profile and cookie (-1 = none or not resident).
struct SceneLightAssets {
    f32 iesIndex = -1.0f;
    f32 cookieIndex = -1.0f;
    f32 iesPeakOverMean = 1.0f;
};

// Irradiance at 1 m from a point/spot source of `candela`, in engine units.
inline f32 pointIrradiance1m(f32 candela) { return candela / kEngineUnitCdm2; }
// Radiance of a rect panel whose intensity along its normal is `candela`: I = L * A.
inline f32 rectRadiance(f32 candela, f32 widthCm, f32 heightCm) {
    const f32 areaM2 = std::max(widthCm * heightCm, 1.0f) * 1e-4f;
    return candela / areaM2 / kEngineUnitCdm2;
}

// Packs a SceneLight. `importanceE1m` gets the 1 m irradiance (colour-weighted) for ranking lights.
inline PackedLight packSceneLight(const SceneLight& s, const SceneLightAssets& a, f32* importanceE1m = nullptr) {
    PackedLight p{};
    const bool rect = s.kind == 3;
    const bool spot = s.kind == 1;
    const u32 kind = rect ? kLightKindRect : (spot ? kLightKindSpot : kLightKindPoint);
    const f32 maxColour = std::max(s.colour[0], std::max(s.colour[1], s.colour[2]));

    // Flux-preserving IES by default (table mean is 1); iesPeak rescales so the profile's peak is 1.
    const f32 iesScale = (a.iesIndex >= 0.0f && s.iesPeak) ? 1.0f / std::max(a.iesPeakOverMean, 1e-6f) : 1.0f;

    const f32 e1m = pointIrradiance1m(s.intensityCd) * iesScale;
    const f32 radiusEff = rect ? 0.0f : std::max(s.radiusCm, 1.0f);
    const f32 halfW = rect ? 0.5f * std::max(s.widthCm, 1.0f) : 0.0f;
    const f32 halfH = rect ? 0.5f * std::max(s.heightCm, 1.0f) : 0.0f;

    f32 range = s.rangeCm;
    if (!(range > 0.0f)) {
        const f32 reach = 100.0f * std::sqrt(std::max(e1m * maxColour, 0.0f) / kLightRangeCutoff);
        const f32 floorCm = rect ? 2.0f * std::max(s.widthCm, s.heightCm) : radiusEff * 4.0f;
        range = std::min(std::max(reach, floorCm), kLightAutoRangeMaxCm);
    }
    range = std::min(range, kLightRangeMaxCm);

    for (int i = 0; i < 3; ++i) p.posRadius[i] = s.pos[i];
    p.posRadius[3] = radiusEff;
    const f32 scale = rect ? rectRadiance(s.intensityCd, s.widthCm, s.heightCm) * iesScale : e1m;
    for (int i = 0; i < 3; ++i) p.radianceRange[i] = s.colour[i] * scale;
    p.radianceRange[3] = range;
    for (int i = 0; i < 3; ++i) p.axisKind[i] = s.axis[i];
    p.axisKind[3] = static_cast<f32>(kind + (s.castShadows ? 0u : kLightNoShadowBit));
    p.shape[0] = spot ? std::min(std::max(s.innerCos, s.outerCos + 1e-3f), 1.0f) : halfW;
    p.shape[1] = spot ? s.outerCos : halfH;
    p.shape[2] = a.iesIndex;
    p.shape[3] = a.cookieIndex;
    for (int i = 0; i < 3; ++i) p.right[i] = s.right[i];
    // Cookie frustum half-angle: the cone's outer angle, or 45 degrees when that is degenerate.
    const f32 oc = s.outerCos;
    p.right[3] = (oc > 0.05f && oc < 1.0f) ? std::sqrt(1.0f - oc * oc) / oc : 1.0f;

    if (importanceE1m) *importanceE1m = (rect ? s.intensityCd / kEngineUnitCdm2 * iesScale : e1m) * maxColour;
    return p;
}

// ---- CPU mirror of aver_lights.hlsli's rectangle maths -------------------------------------------

// Clips the quad q (relative to the receiver) to the hemisphere above N; returns the vertex count (<= 5).
inline u32 clipQuadToHorizon(const Vec3 q[4], const Vec3& N, Vec3 out[5]) {
    u32 n = 0;
    for (u32 i = 0; i < 4; ++i) {
        const Vec3& a = q[i];
        const Vec3& b = q[(i + 1) & 3];
        const f32 da = dot(a, N), db = dot(b, N);
        if (da >= 0.0f) out[n++] = a;
        if ((da >= 0.0f) != (db >= 0.0f)) out[n++] = lerp(a, b, da / (da - db));
    }
    return n;
}

// Vector irradiance per unit radiance of a Lambertian polygon (Lambert / Arvo): dot(result, N) is the
// diffuse irradiance, up to the sign of the winding.
inline Vec3 polygonVectorIrradiance(const Vec3* v, u32 n) {
    Vec3 sum{0, 0, 0};
    for (u32 i = 0; i < n; ++i) {
        const Vec3 a = v[i];
        const Vec3 b = v[(i + 1) % n];
        const Vec3 cr = cross(a, b - a);   // a x b, with its digits kept for a small distant polygon
        const f32 s2 = dot(cr, cr);
        if (s2 > 1e-14f * dot(a, a) * dot(b, b)) sum += cr * (std::atan2(std::sqrt(s2), dot(a, b)) / std::sqrt(s2));
    }
    return sum * 0.5f;
}

// The rect's contribution at p for a surface normal N: `vec` is the vector irradiance per unit radiance
// (oriented toward the light), false when p is behind the panel or the panel is entirely below the horizon.
inline bool rectVectorIrradiance(const PackedLight& l, const Vec3& p, const Vec3& N, Vec3& vec) {
    const Vec3 centre{l.posRadius[0], l.posRadius[1], l.posRadius[2]};
    const Vec3 axis{l.axisKind[0], l.axisKind[1], l.axisKind[2]};
    const Vec3 right{l.right[0], l.right[1], l.right[2]};
    const Vec3 up = cross(axis, right);
    const Vec3 toC = centre - p;
    if (dot(toC * -1.0f, axis) <= 0.0f) return false;
    const Vec3 ex = right * l.shape[0], ey = up * l.shape[1];
    const Vec3 q[4] = {toC + ex + ey, toC - ex + ey, toC - ex - ey, toC + ex - ey};
    Vec3 poly[5];
    const u32 n = clipQuadToHorizon(q, N, poly);
    if (n < 3) return false;
    Vec3 v = polygonVectorIrradiance(poly, n);
    if (dot(v, axis) > 0.0f) v = v * -1.0f;
    if (!(v.size() > 1e-7f)) return false;
    vec = v;
    return true;
}

// Diffuse irradiance (N.L included) of a rect at p, per unit radiance; 0 when it does not reach.
inline f32 rectDiffuseFormFactor(const PackedLight& l, const Vec3& p, const Vec3& N) {
    Vec3 v;
    if (!rectVectorIrradiance(l, p, N, v)) return 0.0f;
    return std::max(dot(v, N), 0.0f);
}

} // namespace aver::voxi
