add_executable(FormatTest src/FormatTest.cpp)
target_link_libraries(FormatTest PRIVATE Aver.Formats Aver.Assets Aver.Core Aver.Platform)
set_target_properties(FormatTest PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
