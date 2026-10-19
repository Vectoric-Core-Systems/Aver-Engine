# Aver.Vehicle.Scene -- drivable vehicles on scene entities, over Jolt's vehicle constraint.
#
# A SECOND TARGET above Aver.Vehicle for the reason Aver.SoftBody.Scene sits above Aver.SoftBody: the pure library (the
# .ocvehicle asset, the spec, the presets, the builder) must stay Core + Formats + the physics C ABI, while owning a vehicle per
# entity needs the scene (components, transforms), and writing the entity's pose every frame in C++ needs nothing more.
#
# It links NONE of audio, particles, soft bodies, cameras or the framework: sound, skid smoke and sparks, crash damage and the
# camera leave through sinks the host installs (VehiclePhysicsSystem.hpp), and the relay (framework_vehicle_abi.h) is a provider
# function the host hands to the framework. Only a composition root links this.
aver_add_module(Aver.Vehicle.Scene
  SOURCES
    src/VehiclePhysicsSystem.cpp
    src/VehicleDriver.cpp
    src/VehicleVisuals.cpp
    src/VehicleAudio.cpp
    src/VehicleProvider.cpp
  DEPS
    Aver.Vehicle
    Aver.Physics
    Aver.Scene
    Aver.Formats
    Aver.Core
  PUBLIC_DEFS
    AVER_MODULE_VEHICLE_SCENE=1
)
