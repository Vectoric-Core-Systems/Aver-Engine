# InputBridgeTest -- the ImGui-free input path, end to end.
#
# Feeds synthetic window events through InputState, publishes into the framework, and reads the
# framework's own state back through the same C ABI gameplay uses. No window, no device, no ImGui
# and no human, which is the point: the defect it guards (an AVER_ENABLE_UI=OFF build having no
# input at all) compiles cleanly and only shows up when somebody presses a key.
add_executable(InputBridgeTest src/InputBridgeTest.cpp)
target_link_libraries(InputBridgeTest PRIVATE Aver.Runtime.Game.Core Aver.Framework Aver.Platform Aver.Core)
set_target_properties(InputBridgeTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
