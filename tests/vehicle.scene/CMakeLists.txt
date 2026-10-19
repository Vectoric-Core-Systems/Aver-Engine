# VehicleSystemTest -- drivable vehicles on scene entities, headless. Specs, Drive.* actions, audio and every sink (effects, crash,
# camera, notify, rig inputs) are injected and nothing is drawn, so the whole system is decidable with no GPU and no device: a
# vehicle built from a spec, its pose written to the entity, wheel visuals on children and rig inputs, a driver getting in and
# out (mesh hidden, character gravity and strength taken and given back, the camera sink's Enter / Next / Exit), telemetry, crash
# damage and repair, smoke that follows time, the nearest eight heard, the distance LOD and replay equality. Physics is the real
# Jolt world, through Aver.Physics.
add_executable(VehicleSystemTest src/VehicleSystemTest.cpp)
target_include_directories(VehicleSystemTest PRIVATE src)
target_link_libraries(VehicleSystemTest PRIVATE Aver.Vehicle.Scene Aver.Vehicle Aver.Physics Aver.Scene Aver.Formats Aver.Core)
set_target_properties(VehicleSystemTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
