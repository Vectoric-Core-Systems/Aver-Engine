# Prefab tests. No add_test here: aver_register_ctest_in (root CMakeLists) registers every target whose
# name ends in Test, with the timeout and the FAIL_REGULAR_EXPRESSION these suites are written against.

# OcPrefabTest -- the .ocprefab format and the prefab records inside .ocworld, including that a level
# with no prefabs is untouched. Formats only: no scene, no GPU.
add_executable(OcPrefabTest src/OcPrefabTest.cpp)
target_link_libraries(OcPrefabTest PRIVATE Aver.Formats Aver.Core Aver.Platform)
set_target_properties(OcPrefabTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# PrefabSystemTest -- instances in a live world: overrides, revert, apply, propagation (nested too),
# undo state, level round trip. The scene world only; no renderer, no framework, no editor.
add_executable(PrefabSystemTest src/PrefabSystemTest.cpp)
target_link_libraries(PrefabSystemTest PRIVATE Aver.Prefab Aver.Scene Aver.Formats Aver.Core Aver.Platform)
set_target_properties(PrefabSystemTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# PrefabEditorModelTest -- the editor's prefab model (create from a selection, place, apply, unpack, the
# override rows), with no UI. It COMPILES the sandbox translation unit directly rather than linking a
# library, the same unusual line EditorPrefsTest has, because the editor's code has no business being
# reachable from the engine just to be testable.
add_executable(PrefabEditorModelTest
  src/PrefabEditorModelTest.cpp
  ${CMAKE_SOURCE_DIR}/sandbox/src/PrefabEditorModel.cpp)
target_include_directories(PrefabEditorModelTest PRIVATE ${CMAKE_SOURCE_DIR}/sandbox/src)
target_link_libraries(PrefabEditorModelTest PRIVATE Aver.Prefab Aver.Scene Aver.Formats Aver.Core Aver.Platform)
set_target_properties(PrefabEditorModelTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
