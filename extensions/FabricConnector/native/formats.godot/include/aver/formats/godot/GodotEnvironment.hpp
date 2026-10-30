#pragma once
// Godot Environment / CameraAttributes parameters -> Aver post, fog and sky parameters (docs/plans/fabric/G4_RENDER.md R3). Pure functions, shared by the
// converter (tools/fabric) and its tests; the C# side (Aver.Fabric EnvironmentMapping) repeats them and a managed test pins the same values.
#include <algorithm>
#include <cmath>

namespace aver::fmt::godotenv {

inline constexpr double kPi = 3.14159265358979323846;

// Aver's tonemap mode for Godot's Environment.tonemap_mode (0 linear, 1 Reinhard, 2 filmic, 3 ACES, 4 AgX): 3 + mode (rhi/Tonemap.hpp).
inline constexpr int kTonemapBase = 3;
inline int tonemapMode(int godotMode) { return kTonemapBase + std::clamp(godotMode, 0, 4); }

// A colour Godot authors as a nominal sRGB value, scaled by an energy multiplier in LINEAR light and returned in the same encoding
// (Aver's authored sky colours are decoded with a 2.2 power in the shader, so the round trip is c * energy^(1/2.2)).
inline double scaleAuthoredColour(double c, double energy) { return c * std::pow(std::max(energy, 0.0), 1.0 / 2.2); }

// Godot's final exposure is Environment.tonemap_exposure times the camera attributes' exposure multiplier.
inline double exposure(double tonemapExposure, double cameraMultiplier) { return std::max(tonemapExposure, 0.0) * std::max(cameraMultiplier, 0.0); }

// Godot's fog_density (exponential, per metre) as Aver's fog density (per centimetre).
inline double fogDensityPerCm(double godotDensity) { return std::max(godotDensity, 0.0) / 100.0; }

// Godot's height fog: fog_height is the world height (metres) the effect starts at and fog_height_density the exponential rate (per metre).
// Aver thins fog going up at `falloff` per centimetre from the density at `heightCm`. Approximated (Godot's term is added to the base fog).
struct HeightFog { double falloffPerCm; double heightCm; };
inline HeightFog heightFog(double fogHeightM, double heightDensity) { return {std::fabs(heightDensity) / 100.0, fogHeightM * 100.0}; }

// Depth-of-field. Aver's lens is physical (focus distance, f-stop, focal length on a 36 mm sensor); Godot's CameraAttributesPractical blurs by
// distance bands, CameraAttributesPhysical is a real lens. Practical becomes a lens focused at the near edge of the far blur band whose
// aperture follows dof_blur_amount (f/4 at Godot's default 0.1). Approximated.
struct Dof { bool on = false; double focusCm = 1000, fStop = 4, focalMm = 35; };
inline Dof dofFromPractical(bool nearOn, bool farOn, double nearM, double farM, double amount) {
    Dof d;
    d.on = nearOn || farOn;
    if (!d.on) return d;
    d.focusCm = std::max(farOn ? farM : nearM, 0.1) * 100.0;
    d.fStop = std::clamp(0.4 / std::max(amount, 1e-3), 0.7, 32.0);
    return d;
}
inline Dof dofFromPhysical(double focusM, double focalMm, double aperture) {
    Dof d;
    d.on = true;
    d.focusCm = std::max(focusM, 0.1) * 100.0;
    d.fStop = std::clamp(aperture, 0.7, 32.0);
    d.focalMm = std::clamp(focalMm, 10.0, 300.0);
    return d;
}

}  // namespace aver::fmt::godotenv
