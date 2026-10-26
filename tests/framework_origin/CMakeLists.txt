# Floating-origin C ABI (framework_origin_abi.h): the provider relay, the answers with no provider, and the WorldPos conversions
# against aver/world/WorldPos.hpp (header-only, so this links the Aver.Framework DLL and Core only). The event payload and the
# request queue are covered by OriginParticipantsTest, which has the real participants.
# NO add_test: aver_register_ctest_in (root CMakeLists) picks up *Test executables.
add_executable(FrameworkOriginAbiTest src/FrameworkOriginAbiTest.cpp)
target_link_libraries(FrameworkOriginAbiTest PRIVATE Aver.Framework Aver.Core)
target_include_directories(FrameworkOriginAbiTest PRIVATE ${CMAKE_SOURCE_DIR}/modules/world/include)
set_target_properties(FrameworkOriginAbiTest PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
