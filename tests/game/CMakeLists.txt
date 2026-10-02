# InputBridgeTest -- the ImGui-free input path, end to end.
#
# Feeds synthetic window events through InputState, publishes into the framework, and reads the
# framework's own state back through the same C ABI gameplay uses. No window, no device, no ImGui
# and no human, which is the point: the defect it guards (an AVER_ENABLE_UI=OFF build having no
# input at all) compiles cleanly and only shows up when somebody presses a key.
add_executable(InputBridgeTest src/InputBridgeTest.cpp)
target_link_libraries(InputBridgeTest PRIVATE Aver.Runtime.Game.Core Aver.Framework Aver.Platform Aver.Core)
set_target_properties(InputBridgeTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# PlayMobilityTest -- which entities count as moving during a play session. The rule's own cases, then a
# few hundred frames of random moves, spawns, deaths, reparenting and pawn changes compared against the
# rule as first written (hash every matrix, walk every ancestor chain), so the cheap version cannot have
# changed an answer. Needs a real scene::World and nothing else: no device, no window.
add_executable(PlayMobilityTest src/PlayMobilityTest.cpp)
target_link_libraries(PlayMobilityTest PRIVATE Aver.Runtime.Game.Core Aver.Scene Aver.Core)
set_target_properties(PlayMobilityTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
