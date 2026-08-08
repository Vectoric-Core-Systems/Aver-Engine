# EditorPrefsTest -- the editor's own preferences store.
#
# It COMPILES the sandbox translation unit directly rather than linking a library, because the store
# lives in the editor and has no business being reachable from the engine. Promoting it to a module
# purely to make it testable would put editor UI state into the engine's dependency graph, which is a
# worse trade than this one unusual line.
add_executable(EditorPrefsTest
  src/EditorPrefsTest.cpp
  ${CMAKE_SOURCE_DIR}/sandbox/src/EditorPrefs.cpp)
target_link_libraries(EditorPrefsTest PRIVATE Aver.Platform Aver.Core)
set_target_properties(EditorPrefsTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# EditorEulerTest -- the Euler <-> quaternion pair the editor reads and writes every transform
# through. Header-only, so this needs no sandbox .cpp; it exists because those two functions used to
# be static inside SandboxApp.cpp where nothing could reach them, and one of them was wrong in a way
# that wrote corrupted rotations into saved levels.
#
# The pair itself now lives in modules/world (it is the .ocworld rotation contract, not an editor
# convention -- the game runtime had been carrying its own copy to read levels the editor wrote), and
# sandbox/src/EditorEuler.hpp re-exports it. This test is UNCHANGED and still links no module: the
# world header is header-only and Core-only, so an include directory is all it needs. That is
# precisely the property that let it move without making the trade this file argues against above.
add_executable(EditorEulerTest src/EditorEulerTest.cpp)
target_include_directories(EditorEulerTest PRIVATE
  ${CMAKE_SOURCE_DIR}/sandbox/src
  ${CMAKE_SOURCE_DIR}/modules/world/include)
target_link_libraries(EditorEulerTest PRIVATE Aver.Core Aver.Platform)
set_target_properties(EditorEulerTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
