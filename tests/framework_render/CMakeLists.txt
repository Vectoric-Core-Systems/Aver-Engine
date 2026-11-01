# Render settings and statistics (framework_render_abi.h, P17 and P18) through the Aver.Framework DLL, and the shared apply that puts a script's keys onto a
# voxi::Settings (aver/voxi/ScriptRenderSettings.hpp) with a fake Api. Headless: no device.
# NO add_test: aver_register_ctest_in (root CMakeLists) picks up *Test executables.
add_executable(FrameworkRenderAbiTest src/FrameworkRenderAbiTest.cpp)
target_link_libraries(FrameworkRenderAbiTest PRIVATE Aver.Framework Aver.Core)
if(TARGET Aver.Render.Voxi)
  target_link_libraries(FrameworkRenderAbiTest PRIVATE Aver.Render.Voxi Aver.Formats)
  target_compile_definitions(FrameworkRenderAbiTest PRIVATE AVER_TEST_WITH_VOXI=1)
endif()
set_target_properties(FrameworkRenderAbiTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
