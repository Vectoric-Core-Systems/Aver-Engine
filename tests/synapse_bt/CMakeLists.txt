# Blackboard + behaviour-tree editor tests (docs/BLACKBOARD_BT.md). A separate directory from
# tests/synapse so the two can grow without sharing a file. Added from the root CMakeLists beside
# tests/synapse, under the same Aver.Synapse guard.

# BlackboardTest -- typed keys, coercion, stamps, observers, agent/team scope isolation. Pure.
add_executable(BlackboardTest src/BlackboardTest.cpp)
target_link_libraries(BlackboardTest PRIVATE Aver.Synapse Aver.Formats Aver.Core Aver.Platform)
set_target_properties(BlackboardTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# BtDecoratorTest -- decorator gating, observer aborts (Self / LowerPriority / Both), abort
# callbacks, the Bb leaves, the per-node trace. Pure: hand-built trees, fake leaves.
add_executable(BtDecoratorTest src/BtDecoratorTest.cpp)
target_link_libraries(BtDecoratorTest PRIVATE Aver.Synapse Aver.Formats Aver.Core Aver.Platform)
set_target_properties(BtDecoratorTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# BtAssetTest -- the .ocbt asset round trip (bytes and file), compatibility with plain .ocbt in both
# directions, corrupt-chunk refusal, structural edits, key renames, validation. Pure.
add_executable(BtAssetTest src/BtAssetTest.cpp)
target_link_libraries(BtAssetTest PRIVATE Aver.Synapse Aver.Formats Aver.Core Aver.Platform)
set_target_properties(BtAssetTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# AiDebugTest -- the AI debug feed's shapes and gating. Header-only, Core only.
add_executable(AiDebugTest src/AiDebugTest.cpp)
target_include_directories(AiDebugTest PRIVATE ${CMAKE_SOURCE_DIR}/modules/synapse/include)
target_link_libraries(AiDebugTest PRIVATE Aver.Core)
set_target_properties(AiDebugTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# BtBlackboardSystemTest -- BtSystem boards, team boards, the scalar relay, perception sync, reload
# and the live-debug view, against a real scene::World.
if(TARGET Aver.Synapse.Scene)
  add_executable(BtBlackboardSystemTest src/BtBlackboardSystemTest.cpp)
  target_link_libraries(BtBlackboardSystemTest PRIVATE Aver.Synapse.Scene Aver.Synapse Aver.Scene
                                                       Aver.Formats Aver.Core Aver.Platform)
  set_target_properties(BtBlackboardSystemTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
endif()

# BtGraphEditorTest -- the visual behaviour-tree editor tab's headless core (layout maths, edit
# bookkeeping, undo, save/load) and the AI debug overlay's geometry. Same arrangement as
# tests/editor's BtEditorTest: Sandbox is add_executable-only, so the sandbox sources are compiled
# here directly with AVER_WITH_IMGUI undefined.
if(TARGET Aver.RHI)
  add_executable(BtGraphEditorTest
    src/BtGraphEditorTest.cpp
    ${CMAKE_SOURCE_DIR}/sandbox/src/BtGraphEditor.cpp
    ${CMAKE_SOURCE_DIR}/sandbox/src/AiDebugOverlay.cpp)
  target_include_directories(BtGraphEditorTest PRIVATE ${CMAKE_SOURCE_DIR}/sandbox/src)
  target_link_libraries(BtGraphEditorTest PRIVATE Aver.Synapse Aver.Formats Aver.Core Aver.Platform Aver.RHI)
  set_target_properties(BtGraphEditorTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
endif()
