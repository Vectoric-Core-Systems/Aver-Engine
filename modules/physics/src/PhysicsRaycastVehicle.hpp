// PhysicsRaycastVehicle.hpp -- the raycast vehicle (G2, Godot's VehicleBody3D). SRC-LOCAL like PhysicsInternal.hpp. The ABI is
// physics_raycast_vehicle_abi.h; the model and its limits are in docs/physics/GODOT_VEHICLE.md. Every hook is a pointer test when nothing uses it.
#pragma once
#include "PhysicsInternal.hpp"

namespace aver::physics::detail {

// Fixed loop, before system.Update: ray per wheel, suspension and friction impulses on the chassis (and on a dynamic ground).
void rayVehicleStepBegin(float dt);

// aver_phys_remove_body, before the body goes: a vehicle on this chassis is destroyed.
void rayVehicleOnBodyRemoved(int32_t handle);

} // namespace aver::physics::detail
