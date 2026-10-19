# Aver.Vehicle -- the .ocvehicle asset, the spec, the presets and the builder, with no scene and no GPU.
#
# OcVehicleTest is pure text and arithmetic (parse, write, validate, the traffic derivation, the parse cache) and
# links Aver.Vehicle + Core only; with AVER_REPO_ROOT it also reads the template's and the lab's .ocvehicle files from
# the tree and holds them to the same rules. VehicleBuildTest builds every preset and the shipped specs through the
# real Jolt world (Aver.Physics) and drives them: observed motion, as tests/physics does.
add_executable(OcVehicleTest src/OcVehicleTest.cpp)
target_link_libraries(OcVehicleTest PRIVATE Aver.Vehicle Aver.Formats Aver.Core)
target_compile_definitions(OcVehicleTest PRIVATE AVER_REPO_ROOT="${CMAKE_SOURCE_DIR}")
set_target_properties(OcVehicleTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

add_executable(VehicleBuildTest src/VehicleBuildTest.cpp)
target_link_libraries(VehicleBuildTest PRIVATE Aver.Vehicle Aver.Physics Aver.Formats Aver.Core)
set_target_properties(VehicleBuildTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
