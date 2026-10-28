#pragma once
// Godot light units -> Aver light units, and Godot's documented attenuation curves. One place for the calibration so the converter
// (tools/fabric), the raster local-light path (render.voxi RasterLights.hpp) and Aver.Fabric's Light3D agree; the C# side repeats the two
// constants and a test compares them. Reasoning and formulas: docs/plans/fabric/G4_RENDER.md R2.
#include <algorithm>
#include <cmath>

namespace aver::fmt::godotlight {

inline constexpr double kPi = 3.14159265358979323846;
// One engine radiance unit in cd/m^2 (rhi::kLuminanceToCdm2 = voxi::kEngineUnitCdm2): the 100000 lux sun is 3.0 of them.
inline constexpr double kEngineUnitCdm2 = 100000.0 / 3.0;

// Godot (non-physical units) multiplies a light's energy by pi, and its diffuse BRDF carries 1/pi, so a white surface facing a light of
// energy E returns E. Aver's diffuse BRDF also carries 1/pi and its sun is an irradiance in engine units, so the irradiance that
// reproduces "E" is pi * E engine units = pi * E * kEngineUnitCdm2 lux (sun) or candela (a point/spot light's irradiance at 1 m).
inline constexpr double kLuxPerEnergy = kPi * kEngineUnitCdm2;       // 104719.755
inline constexpr double kCandelaPerEnergy = kPi * kEngineUnitCdm2;   // at 1 m; Godot's attenuation is relative to 1 m as well

inline double sunLux(double energy) { return energy * kLuxPerEnergy; }
inline double candela(double energy) { return energy * kCandelaPerEnergy; }
// Godot's specular_amount is light_specular * 2 (light_storage.cpp), so Godot's default light_specular 0.5 is a factor 1.
inline double specularScale(double lightSpecular) { return std::max(lightSpecular, 0.0) * 2.0; }

// get_omni_attenuation (scene_forward_lights_inc.glsl): linear window times distance^-decay, distance in metres.
inline double omniAttenuation(double distanceM, double rangeM, double decay) {
    const double nd = std::max(1.0 - distanceM / std::max(rangeM, 1e-3), 0.0);
    return nd * std::pow(std::max(distanceM, 1e-4), -decay);
}
// Angular factor of a spot: 1 - rim^(1/spot_angle_attenuation), rim = (1 - cos) / (1 - cos(spot_angle)), cos clamped to the cone's edge.
inline double spotCone(double cosToAxis, double spotAngleDeg, double angleAttenuation) {
    const double coneCos = std::cos(spotAngleDeg * kPi / 180.0);
    const double scos = std::max(cosToAxis, coneCos);
    const double rim = std::max(1e-4, (1.0 - scos) / (1.0 - coneCos));
    return 1.0 - std::pow(rim, 1.0 / std::max(angleAttenuation, 1e-3));
}

}  // namespace aver::fmt::godotlight
