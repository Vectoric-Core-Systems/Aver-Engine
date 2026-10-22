# Jelly, rope, grab, buoyancy and contact C ABI (framework_softshape_abi.h): the relay to an installed provider, the zeros without
# one, and the refusals that never reach the provider. Links the Aver.Framework DLL only: the provider is a fake here, so no soft-body
# module is needed.
#
# SoftShapeDetailsModelTest is the ImGui-free half of the Jelly and Rope Details sections (sandbox/src/SoftShapeDetailsModel.hpp). It is
# header-only, so it needs the sandbox include directory and Aver.Formats / Aver.SoftBody only; no editor target.
# NO add_test: aver_register_ctest_in (root CMakeLists) picks up *Test executables.
add_executable(FrameworkSoftShapeAbiTest src/FrameworkSoftShapeAbiTest.cpp)
target_link_libraries(FrameworkSoftShapeAbiTest PRIVATE Aver.Framework Aver.Core)
set_target_properties(FrameworkSoftShapeAbiTest PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

add_executable(SoftShapeDetailsModelTest src/SoftShapeDetailsModelTest.cpp)
target_include_directories(SoftShapeDetailsModelTest PRIVATE ${CMAKE_SOURCE_DIR}/sandbox/src)
target_link_libraries(SoftShapeDetailsModelTest PRIVATE Aver.SoftBody Aver.Formats Aver.Core Aver.Platform)
set_target_properties(SoftShapeDetailsModelTest PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
