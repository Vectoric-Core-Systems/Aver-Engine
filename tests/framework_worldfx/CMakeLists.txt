# World effects C ABI (framework_worldfx_abi.h): sun / fog / sky-light overrides, post layers and volume scales, the
# blend that advances with aver_fw_worldfx_tick, and the zeros for bad arguments. Links the Aver.Framework DLL only.
# NO add_test: aver_register_ctest_in (root CMakeLists) picks up *Test executables.
add_executable(FrameworkWorldFxAbiTest src/FrameworkWorldFxAbiTest.cpp)
target_link_libraries(FrameworkWorldFxAbiTest PRIVATE Aver.Framework Aver.Formats Aver.Core)
set_target_properties(FrameworkWorldFxAbiTest PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
