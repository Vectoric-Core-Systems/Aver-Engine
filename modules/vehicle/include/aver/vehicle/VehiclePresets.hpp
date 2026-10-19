#pragma once
// The built-in vehicles. Two kinds, kept apart on purpose:
//
//   * the PRESETS the traffic system has always had (car van truck bus sports), plus the tank: no geometry of
//     their own, sized from a mesh's bounds by specFromBounds with the formulas world::VehicleSystem used to hold;
//   * the FIXED-GEOMETRY specs the editor offers as starting points (sedan, tank), complete vehicles with named
//     wheel nodes, exactly what the Vehicle template ships.
#include "aver/vehicle/VehicleSpec.hpp"

#include <string_view>

namespace aver::vehicle {

// Preset numbers for CVehicle::preset and the `vehicle <name>` token: 0 car, 1 van, 2 truck, 3 bus, 4 sports, 5 tank.
inline constexpr u32 kPresetCount = 6;
const char* presetName(u32 preset);                 // null past the end
int presetIndex(std::string_view name);             // -1 for a name that is not a preset (case-insensitive)

// The fixed-geometry specs ("sedan", "tank"), or null. Process-lifetime objects.
const VehicleSpec* builtinSpec(std::string_view name);

// The traffic derivation, moved here: the spec for `preset` sized to a mesh of `sizeCm` (length, width, height of
// its bounds times its scale). A four-wheeler gets its wheels from the formulas of the old kPresets table, rear
// handbrake, front steer; "tank" is tracked with five road wheels a side. False for a name that is not a preset or
// a size that is not positive.
bool specFromBounds(std::string_view preset, const Vec3& sizeCm, VehicleSpec& out);

} // namespace aver::vehicle
