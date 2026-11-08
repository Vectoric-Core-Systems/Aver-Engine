# Physical animation C ABI (framework_physanim_abi.h): the relay to an installed provider and the zeros without one.
# Links the Aver.Framework DLL only: the provider is a fake here, so no ragdoll module is needed.
# NO add_test: aver_register_ctest_in (root CMakeLists) picks up *Test executables.
add_executable(FrameworkPhysAnimAbiTest src/FrameworkPhysAnimAbiTest.cpp)
target_link_libraries(FrameworkPhysAnimAbiTest PRIVATE Aver.Framework Aver.Core)
set_target_properties(FrameworkPhysAnimAbiTest PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
