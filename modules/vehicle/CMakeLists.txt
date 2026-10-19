# Aver.Vehicle -- the pure half of vehicles: the .ocvehicle asset, the spec, the built-in presets and the
# builder that turns a spec into the physics C ABI's calls (physics_vehicle_abi.h and physics_vehicle_ext_abi.h).
#
# PURE BY CONSTRUCTION: no scene, no GPU, no Jolt header. It links Aver.Physics for the C ABI only, so
# tests/vehicle decides parsing, validation, the preset derivation and the builder on a machine with no device.
# The component, the per-frame system, wheel visuals, audio and the camera / effect / crash seams are
# Aver.Vehicle.Scene (modules/vehicle.scene); the traffic system (Aver.World) reads this module's presets.
#
# Added by the root CMakeLists.txt inside if(AVER_MODULE_PHYSICS), after modules/physics.
aver_add_module(Aver.Vehicle
  SOURCES
    src/OcVehicle.cpp
    src/VehiclePresets.cpp
    src/VehicleBuild.cpp
  DEPS
    Aver.Core
    Aver.Formats
    Aver.Physics
  PUBLIC_DEFS
    AVER_MODULE_VEHICLE=1
)
