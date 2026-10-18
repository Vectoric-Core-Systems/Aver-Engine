# Blend-shape / facial-animation C ABI (framework_morph_abi.h): the relay to an installed provider and the zeros without one.
# Links the Aver.Framework DLL only: the provider is a fake here, so no animation module is needed.
# NO add_test: aver_register_ctest_in (root CMakeLists) picks up *Test executables.
add_executable(FrameworkMorphAbiTest src/FrameworkMorphAbiTest.cpp)
target_link_libraries(FrameworkMorphAbiTest PRIVATE Aver.Framework Aver.Core)
set_target_properties(FrameworkMorphAbiTest PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
