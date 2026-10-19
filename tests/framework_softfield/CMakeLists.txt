# Soft-body field C ABI (framework_softfield_abi.h): the relay to an installed provider, the zeros without one, and the refusals that
# never reach the provider. Links the Aver.Framework DLL only: the provider is a fake here, so no soft-body module is needed.
# NO add_test: aver_register_ctest_in (root CMakeLists) picks up *Test executables.
add_executable(FrameworkSoftFieldAbiTest src/FrameworkSoftFieldAbiTest.cpp)
target_link_libraries(FrameworkSoftFieldAbiTest PRIVATE Aver.Framework Aver.Core)
set_target_properties(FrameworkSoftFieldAbiTest PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
