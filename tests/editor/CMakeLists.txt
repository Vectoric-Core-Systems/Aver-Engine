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

# GraphEditorGeometryTest -- the .ocgraph node editor's headless core: the node descriptor table
# (sandbox/src/GraphNodeDefs.hpp), and layout/hit-testing/link-rules/cycle-detection/canvas-transform
# (sandbox/src/GraphEditorGeometry.hpp/.cpp). No ImGui, no Engine, no scene:: -- an editor that only
# edits a text file needs none of those, and keeping it that way is what lets this run headless.
#
# COMPILES sandbox/src/GraphEditorGeometry.cpp directly into the test binary rather than linking a
# library, for EditorPrefsTest's exact reason above: Sandbox is add_executable-only (no library half),
# and promoting editor geometry into a real module purely to make it testable would put editor UI code
# into the engine's dependency graph, which is a worse trade than this one unusual line.
# GraphNodeDefs.hpp is header-only (see its own header comment) and needs no extra source file.
add_executable(GraphEditorGeometryTest
  src/GraphEditorGeometryTest.cpp
  ${CMAKE_SOURCE_DIR}/sandbox/src/GraphEditorGeometry.cpp)
target_include_directories(GraphEditorGeometryTest PRIVATE
  ${CMAKE_SOURCE_DIR}/sandbox/src)
target_link_libraries(GraphEditorGeometryTest PRIVATE Aver.Formats Aver.Core Aver.Platform)
set_target_properties(GraphEditorGeometryTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# GraphEditorLoadSaveTest -- the .ocgraph editor TAB's public surface: construct from a path, save().
# Same EditorPrefsTest-precedent reasoning as GraphEditorGeometryTest above (Sandbox is
# add_executable-only), so this compiles sandbox/src/GraphEditor.cpp and GraphEditorGeometry.cpp
# directly into the test binary rather than linking a library.
#
# AVER_WITH_IMGUI is deliberately left UNDEFINED for this target: GraphEditor.cpp's `#include
# "imgui.h"` and its entire draw() method are behind `#if AVER_WITH_IMGUI` (see that file), so this
# target never needs ImGui headers or an ImGui context, and genuinely exercises load/save/undo/
# selection headlessly -- the part of the editor that is NOT gated behind a window.
#
# AVER_OCGRAPH_FIXTURE reuses the exact fixture path/definition tests/formats/CMakeLists.txt already
# established for OcGraphTest's own cross-impl check, so there is one definition of "where that file
# lives," not two that could drift apart.
add_executable(GraphEditorLoadSaveTest
  src/GraphEditorLoadSaveTest.cpp
  ${CMAKE_SOURCE_DIR}/sandbox/src/GraphEditor.cpp
  ${CMAKE_SOURCE_DIR}/sandbox/src/GraphEditorGeometry.cpp)
target_include_directories(GraphEditorLoadSaveTest PRIVATE
  ${CMAKE_SOURCE_DIR}/sandbox/src)
target_compile_definitions(GraphEditorLoadSaveTest PRIVATE
  AVER_OCGRAPH_FIXTURE="${CMAKE_SOURCE_DIR}/scripting/csharp/Aver.Graph.Tests/cross_impl_test.ocgraph")
target_link_libraries(GraphEditorLoadSaveTest PRIVATE Aver.Formats Aver.Core Aver.Platform)
set_target_properties(GraphEditorLoadSaveTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
