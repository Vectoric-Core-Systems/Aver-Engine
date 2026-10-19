# Aver.Physics — Jolt behind the engine's plain-C ABI (see README.md).
#
# SHARED for the same reason as Aver.Scene and Aver.Framework: the C# layer will P/Invoke into it, and
# one binary means one world and one set of handles.
#
# Aver.Physics.Jolt (modules/physics.jolt) is linked PRIVATE on purpose. It is the BACKEND behind
# this module, exactly as Aver.RHI.D3D12 is a backend behind Aver.RHI: nothing above should be able
# to include a JPH:: header by accident, because the moment something does, swapping the backend
# stops being a decision about this module and becomes a decision about the whole tree.
add_library(Aver.Physics SHARED
  src/PhysicsWorld.cpp
  # Buoyancy. Its own translation units rather than more of PhysicsWorld.cpp because it is the first
  # thing in this module that a caller can turn ON AND OFF -- everything else here is unconditional
  # machinery -- and because keeping the Jolt call in one small file makes it obvious that
  # ApplyBuoyancyImpulse is the whole of the physics, and the rest is bookkeeping.
  src/Buoyancy.cpp
  src/BuoyancyAbi.cpp
  # Body dynamics: forces, impulses, spin, the material properties and mass. Its own TU for the same
  # reason buoyancy is one -- PhysicsWorld.cpp owns the world's lifetime and the step loop, and this
  # is a flat sheet of per-body setters that shares only the handle tables (src/PhysicsInternal.hpp).
  src/PhysicsBody.cpp
  # Joints, and the extra rigid shapes. Same reasoning again: each is a self-contained sheet of ABI
  # entry points over the handle tables in src/PhysicsInternal.hpp, with its own header beside
  # physics_abi.h so the C# parity test can pair one header with one binding file.
  src/PhysicsJoints.cpp
  src/PhysicsShapes.cpp
  # Collision layers, and the _ex forms of the three spatial queries that can filter by them.
  src/PhysicsLayers.cpp
  # The character's own settings: slope, stairs, ground info, and the shape swap that is crouching.
  src/PhysicsCharacter.cpp
  # Wheeled vehicles: Jolt's VehicleConstraint as a builder-style ABI (create the chassis, add wheels,
  # finish). Its own TU and header for the reason joints and characters are: a flat sheet of entry points
  # over the handle tables in src/PhysicsInternal.hpp, with the vehicle table and its teardown hooks
  # (destroyAllVehicles, releaseVehicleOfBody) defined here and called from PhysicsWorld.cpp.
  src/PhysicsVehicle.cpp
  # VEHICLES v2 (physics_vehicle_ext_abi.h): the engine / gearbox / differential / tyre builder calls, the driver
  # aids, aero, telemetry and events the fixed step runs around system.Update (vehicleStepBegin / End), and the
  # tracked vehicle (Jolt's TrackedVehicleController behind the same builder).
  src/PhysicsVehicleExt.cpp
  src/PhysicsVehicleTracked.cpp
  # Ragdolls from a skeleton (physics_ragdoll_abi.h): Jolt's Ragdoll with kinematic, limp and powered modes.
  src/PhysicsRagdoll.cpp
  # Shape casts, overlaps, multi-hit rays and point queries (physics_queries_abi.h).
  src/PhysicsQueries.cpp
  # Per-body extras: locked axes, continuous collision, breakable joints' events (physics_body_ext_abi.h).
  src/PhysicsBodyExt.cpp
  # Builder-style soft bodies for cloth and destruction (physics_softbody_abi.h): the registry that lets
  # a dent be written in place and a tear rebuild the body under the same handle, the soft-body contact
  # listener (impacts), and wind and drag evaluated in the fixed loop beside buoyancy.
  src/PhysicsSoftBody.cpp
  src/SoftBodyAero.cpp)

target_include_directories(Aver.Physics PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(Aver.Physics PUBLIC cxx_std_20)
target_link_libraries(Aver.Physics PUBLIC Aver.Core PRIVATE Aver.Physics.Jolt)

target_compile_definitions(Aver.Physics PRIVATE AVER_PHYS_BUILD)
target_compile_definitions(Aver.Physics PUBLIC AVER_MODULE_PHYSICS=1)

set_target_properties(Aver.Physics PROPERTIES
  FOLDER "modules"
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)   # DLL next to the exe
