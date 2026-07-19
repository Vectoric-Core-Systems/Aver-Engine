add_executable(Sandbox src/SandboxApp.cpp)
target_link_libraries(Sandbox PRIVATE Aver.Runtime)
set_target_properties(Sandbox PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
