# UiTest — the draw list, with no GPU. That is the whole reason Aver.UI is Core-only: batching, clip
# nesting, layer separation and premultiplication are each a thing that looks fine on screen while
# being subtly wrong, and every one of them is decidable on the CPU.
add_executable(UiTest src/UiTest.cpp)
target_link_libraries(UiTest PRIVATE Aver.UI Aver.Core Aver.Platform)
set_target_properties(UiTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# UiWidgetTest — the retained widget system: layout maths, DPI scaling, hit testing, focus order,
# input routing, the value widgets and the ready-made screens. Same reasoning: all of it is
# arithmetic and state, so it is decided with no device and no window.
add_executable(UiWidgetTest src/UiWidgetTest.cpp)
target_link_libraries(UiWidgetTest PRIVATE Aver.UI Aver.Core Aver.Platform)
set_target_properties(UiWidgetTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# UiLayoutAssetTest — the .ocui layout asset: the property table, round trip, parse errors,
# forward compatibility, the editor's structural edits and instantiate/capture.
add_executable(UiLayoutAssetTest src/UiLayoutAssetTest.cpp)
target_link_libraries(UiLayoutAssetTest PRIVATE Aver.UI Aver.Core Aver.Platform)
set_target_properties(UiLayoutAssetTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# UiLayoutEditorTest — the layout editor tab's headless core (load / save / dirty / undo, the tab's
# own edits, drag and nudge maths). Same arrangement as BtEditorTest: Sandbox is add_executable-only,
# so this compiles sandbox/src/UiLayoutEditor.cpp directly with AVER_WITH_IMGUI left undefined; the
# ImGui half of that file sits behind the macro.
add_executable(UiLayoutEditorTest
  src/UiLayoutEditorTest.cpp
  ${CMAKE_SOURCE_DIR}/sandbox/src/UiLayoutEditor.cpp)
target_include_directories(UiLayoutEditorTest PRIVATE ${CMAKE_SOURCE_DIR}/sandbox/src)
target_link_libraries(UiLayoutEditorTest PRIVATE Aver.UI Aver.Core Aver.Platform)
set_target_properties(UiLayoutEditorTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
