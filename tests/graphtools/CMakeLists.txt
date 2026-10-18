# The graph editor's tool models: Problems, Find / Find References, the shared clipboard, Collapse to Function
# and the debugger's model (sandbox/src/Graph{Problems,Find,Clipboard,Collapse,DebugModel}.hpp). All header-only
# and ImGui-free, so these run with no window, no GPU and no .NET runtime. The managed halves
# (GraphDiagnostics, GraphDebug, the collapse proof) are Aver.Graph.Tests' DiagnosticsTests, DebugHookTests
# and CollapseFunctionTests.
# NO add_test: aver_register_ctest_in (root CMakeLists) picks up *Test executables.
set(AVER_GRAPHTOOLS_SRC ${CMAKE_SOURCE_DIR}/sandbox/src)

# Compiles the editor's headless core directly into the test binary, for GraphEditorGeometryTest's reason
# (tests/editor/CMakeLists.txt): Sandbox is an executable with no library half.
function(aver_graphtool_test name)
  add_executable(${name} src/${name}.cpp ${ARGN})
  target_include_directories(${name} PRIVATE ${AVER_GRAPHTOOLS_SRC})
  target_link_libraries(${name} PRIVATE Aver.Formats Aver.Core Aver.Platform)
  set_target_properties(${name} PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
endfunction()

aver_graphtool_test(GraphProblemsTest)
aver_graphtool_test(GraphDebugModelTest)
aver_graphtool_test(GraphFindTest)
aver_graphtool_test(GraphClipboardTest ${AVER_GRAPHTOOLS_SRC}/GraphEditorGeometry.cpp)

# GraphCollapseTest drives the real GraphEditor (headless: AVER_WITH_IMGUI is not defined) so the command
# is proved as one undo step and the saved file is what the C# suite compiles and runs. It reads
# collapse_test.ocgraph and writes collapse_test.output.ocgraph beside it, as GraphEditorLoadSaveTest does
# for attribute_edit_test.
aver_graphtool_test(GraphCollapseTest ${AVER_GRAPHTOOLS_SRC}/GraphEditor.cpp ${AVER_GRAPHTOOLS_SRC}/GraphEditorGeometry.cpp)
target_compile_definitions(GraphCollapseTest PRIVATE
  AVER_COLLAPSE_FIXTURE="${CMAKE_SOURCE_DIR}/scripting/csharp/Aver.Graph.Tests/collapse_test.ocgraph"
  AVER_COLLAPSE_FIXTURE_OUTPUT="${CMAKE_SOURCE_DIR}/scripting/csharp/Aver.Graph.Tests/collapse_test.output.ocgraph")
