# Fog volumes and reflection probes (G4 R8): the pure maths (aver/core/FogVolume.hpp, ReflectionProbe.hpp, header-only, Core only) and the C ABI over it
# (framework_envfx_abi.h through the Aver.Framework DLL). Headless: no device, no scene, no engine run.
# NO add_test: aver_register_ctest_in (root CMakeLists) picks up *Test executables.
add_executable(EnvFxTest src/EnvFxTest.cpp)
target_link_libraries(EnvFxTest PRIVATE Aver.Core)
target_compile_definitions(EnvFxTest PRIVATE AVER_REPO_ROOT="${CMAKE_SOURCE_DIR}")
set_target_properties(EnvFxTest PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

add_executable(FrameworkEnvFxAbiTest src/FrameworkEnvFxAbiTest.cpp)
target_link_libraries(FrameworkEnvFxAbiTest PRIVATE Aver.Framework Aver.Core)
set_target_properties(FrameworkEnvFxAbiTest PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
