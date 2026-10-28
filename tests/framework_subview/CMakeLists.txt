# Sub-viewports (G4 R5): the registry's rules (aver/core/SubViewport.hpp, header-only, Core only) and the C ABI over it (framework_subview_abi.h through
# the Aver.Framework DLL). Headless: no device, no scene, no engine run.
# NO add_test: aver_register_ctest_in (root CMakeLists) picks up *Test executables.
add_executable(SubViewportTest src/SubViewportTest.cpp)
target_link_libraries(SubViewportTest PRIVATE Aver.Core)
set_target_properties(SubViewportTest PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

add_executable(FrameworkSubViewAbiTest src/FrameworkSubViewAbiTest.cpp)
target_link_libraries(FrameworkSubViewAbiTest PRIVATE Aver.Framework Aver.Core)
set_target_properties(FrameworkSubViewAbiTest PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
