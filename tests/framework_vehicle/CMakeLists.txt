# Vehicle C ABI (framework_vehicle_abi.h): the relay to an installed provider and the zeros without one.
# Links the Aver.Framework DLL only: the provider is a fake here, so no vehicle module is needed.
# NO add_test: aver_register_ctest_in (root CMakeLists) picks up *Test executables.
add_executable(FrameworkVehicleAbiTest src/FrameworkVehicleAbiTest.cpp)
target_link_libraries(FrameworkVehicleAbiTest PRIVATE Aver.Framework Aver.Core)
set_target_properties(FrameworkVehicleAbiTest PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
