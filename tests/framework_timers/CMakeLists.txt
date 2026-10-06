# Timers and event bus: ordering, pause, dilation, cancel/add inside a callback, re-entrancy, and the C ABI.
# Header-only service classes are tested directly; the ABI half links the Aver.Framework DLL.
# NO add_test: aver_register_ctest_in (root CMakeLists) picks up *Test executables.
add_executable(FrameworkTimersTest src/FrameworkTimersTest.cpp)
target_link_libraries(FrameworkTimersTest PRIVATE Aver.Framework Aver.Core)
set_target_properties(FrameworkTimersTest PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
