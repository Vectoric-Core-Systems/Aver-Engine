// Which scene lights a `lit` emitter takes: up to four, ranked by irradiance at the emitter's bounds centre, unshadowed.
// Pure functions over scene::WorldLight, so the ranking and the unit conversion are tested without a device.
#pragma once
#include "aver/core/Types.hpp"
#include "aver/scene/LightGather.hpp"

#include <vector>

namespace aver::particles {

inline constexpr u32 kParticleLightCount = 4;

// One light as the pixel shader reads it: four float4s (AverParticleLight in particles.hlsl).
struct ParticleLight {
    f32 posRange[4]   = {0, 0, 0, 0};   // xyz position (cm), w range (cm; 0 = unbounded)
    f32 radiance[4]   = {0, 0, 0, 0};   // rgb colour * irradiance at 1 m in engine radiance units, w source radius (cm)
    f32 axisKind[4]   = {1, 0, 0, 0};   // xyz axis, w kind (0 none, 1 point, 2 spot, 3 rect)
    f32 cone[4]       = {1, 0.7f, 0, 0};   // x innerCos, y outerCos
};
static_assert(sizeof(ParticleLight) == 64, "ParticleLight is four float4s");

// Irradiance at 1 m in engine radiance units: candela / rhi::kLuminanceToCdm2.
f32 particleLightE1m(f32 candela);

// Ranks `lights` by irradiance at `centre` (cm), a sphere of `radiusCm` around it counting as the nearest approach, and
// writes the best four to out[0..3]; unused slots keep zero radiance. Returns how many are real.
u32 selectParticleLights(const std::vector<scene::WorldLight>& lights, const Vec3& centre, f32 radiusCm,
                         ParticleLight out[kParticleLightCount]);

} // namespace aver::particles
