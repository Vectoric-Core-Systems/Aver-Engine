#pragma once
// buildVehicle: the one place a spec becomes physics ABI calls. The traffic system, VehiclePhysicsSystem and the
// tests all go through it, so a vehicle is built the same way wherever it comes from.
//
// It calls the v1 builder (physics_vehicle_abi.h) for what v1 can express and the v2 calls
// (physics_vehicle_ext_abi.h) only for what a spec sets beyond the defaults, so a spec equal to a traffic preset
// issues the same v1 calls with the same arguments as the code it replaced.
#include "aver/core/Math.hpp"
#include "aver/vehicle/VehicleSpec.hpp"

#include <string>

namespace aver::vehicle {

struct BuiltVehicle {
    i32 vehicle = 0;        // the physics vehicle handle
    i32 body = 0;           // its chassis body handle
    u32 wheelCount = 0;
    Kind kind = Kind::Wheeled;
    f32 wheelbase = 0.0f;   // front axle to rear axle, cm (the traffic driver's pure pursuit needs it)
    f32 maxSteerRad = 0.0f; // the largest wheel steer angle
};

// Builds `spec` with the vehicle origin at `origin` (position and rotation; scale is not applied: the physics is
// unscaled). False with `why` set when the spec is not valid or the physics module refuses a step; nothing is left
// behind in the world on failure.
bool buildVehicle(const VehicleSpec& spec, const Transform& origin, BuiltVehicle& out, std::string& why);

// The spring sag g / (2 pi f)^2, cm: how far a loaded spring of `hz` hertz compresses, which the builder subtracts
// from a wheel's attachment point so the settled tyre bottom sits on the origin plane.
f32 springSagCm(f32 hz);

} // namespace aver::vehicle
