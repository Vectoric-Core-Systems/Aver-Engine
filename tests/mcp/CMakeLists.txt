# The wire-protocol parser. NO SOCKET is opened: parseCommand is exposed precisely so the part where
# bugs live can be checked without binding a port, which also means this test runs on a build machine
# with no network and cannot collide with a running editor.
add_executable(McpTest src/McpTest.cpp)
target_link_libraries(McpTest PRIVATE Aver.Mcp Aver.Core)
set_target_properties(McpTest PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
