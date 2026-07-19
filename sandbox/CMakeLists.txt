add_executable(Sandbox src/SandboxApp.cpp)
target_link_libraries(Sandbox PRIVATE Aver.Runtime Aver.Formats)
target_include_directories(Sandbox PRIVATE ${CMAKE_SOURCE_DIR}/third_party/stb)
set_target_properties(Sandbox PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
