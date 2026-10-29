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

# Godot-parity control rendering (docs/plans/fabric/G5_PARITY.md U1): style box tessellation, stretch / letterbox maths and the
# .octheme resource, all CPU-side; UiStyleAbiTest links the DLL Aver.Fabric loads (the style box seam, stretch, default theme).
foreach(_t UiStyleBoxTest UiStretchTest UiThemeSheetTest)
  add_executable(${_t} src/${_t}.cpp)
  target_link_libraries(${_t} PRIVATE Aver.UI Aver.Core Aver.Platform)
  set_target_properties(${_t} PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
endforeach()

add_executable(UiStyleAbiTest src/UiStyleAbiTest.cpp)
target_link_libraries(UiStyleAbiTest PRIVATE Aver.UI.Abi Aver.UI Aver.Core Aver.Platform)
set_target_properties(UiStyleAbiTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

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

# UiLocalizeTest / UiLocalizeAbiTest -- localized widget text: UiTree::shown(), the resolver and its epoch, layout and
# draw using the resolved string, typed text and events staying raw; and the C ABI hook a host installs
# (docs/plans/wave2/DATA_TABLES_LOCALIZATION.md 4.3). No device. The ABI test links the DLL the C# side loads.
add_executable(UiLocalizeTest src/UiLocalizeTest.cpp)
target_link_libraries(UiLocalizeTest PRIVATE Aver.UI Aver.Core Aver.Platform)
set_target_properties(UiLocalizeTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

add_executable(UiLocalizeAbiTest src/UiLocalizeAbiTest.cpp)
target_link_libraries(UiLocalizeAbiTest PRIVATE Aver.UI.Abi Aver.UI Aver.Core Aver.Platform)
set_target_properties(UiLocalizeAbiTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# ---- the text engine (docs/GAME_UI_TEXT.md) ---------------------------------------------------------------
#
# Everything but the pixels is a ctest: Unicode tables and UTF-8, bidi, Arabic shaping, line breaking, the font
# layer over stb_truetype (fonts are built in memory by src/TestFont.hpp), the glyph atlas, the layout engine, the
# widget tree on top of it, and the C ABI. None needs a GPU or a window. UiFontTest, UiTextLayoutTest also read
# Roboto from the repo (AVER_REPO_ROOT) and skip that part when it is absent.
foreach(_t UiUnicodeTest UiBidiTest UiShapingTest UiLineBreakTest UiGlyphAtlasTest UiTextTreeTest)
  add_executable(${_t} src/${_t}.cpp)
  target_link_libraries(${_t} PRIVATE Aver.UI Aver.Core Aver.Platform)
  set_target_properties(${_t} PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
endforeach()

foreach(_t UiFontTest UiTextLayoutTest)
  add_executable(${_t} src/${_t}.cpp)
  target_link_libraries(${_t} PRIVATE Aver.UI Aver.Core Aver.Platform)
  target_compile_definitions(${_t} PRIVATE AVER_REPO_ROOT="${CMAKE_SOURCE_DIR}")
  set_target_properties(${_t} PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
endforeach()

# UiTextAbiTest links the DLL the C# side loads, like UiLocalizeAbiTest.
add_executable(UiTextAbiTest src/UiTextAbiTest.cpp)
target_link_libraries(UiTextAbiTest PRIVATE Aver.UI.Abi Aver.UI Aver.Core Aver.Platform)
set_target_properties(UiTextAbiTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# UiTextEffectTest -- text outline / shadow passes (G5 U2): the ring geometry and the ABI call that draws them. Links the DLL the C# side loads.
add_executable(UiTextEffectTest src/UiTextEffectTest.cpp)
target_link_libraries(UiTextEffectTest PRIVATE Aver.UI.Abi Aver.UI Aver.Core Aver.Platform)
set_target_properties(UiTextEffectTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# FontSetEditorTest -- the .ocfonts font set editor tab's headless core (the document edits, load/save/dirty/undo,
# coverage, the preview engine). Same arrangement as UiLayoutEditorTest above: Sandbox is add_executable-only, so
# this compiles sandbox/src/FontSetEditor.cpp directly with AVER_WITH_IMGUI left undefined. The source lives in
# tests/editor/src beside the other editor tests; it is registered here because this directory already links
# Aver.UI (it also reads the synthetic font builder in src/TestFont.hpp).
add_executable(FontSetEditorTest
  ${CMAKE_SOURCE_DIR}/tests/editor/src/FontSetEditorTest.cpp
  ${CMAKE_SOURCE_DIR}/sandbox/src/FontSetEditor.cpp)
target_include_directories(FontSetEditorTest PRIVATE ${CMAKE_SOURCE_DIR}/sandbox/src)
target_link_libraries(FontSetEditorTest PRIVATE Aver.UI Aver.Core Aver.Platform)
set_target_properties(FontSetEditorTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# UiAccessTest / UiCaptionViewTest -- accessibility in the retained UI (docs/ACCESSIBILITY.md): text scale, high contrast,
# the spoken form of every widget kind, the settings screen's new tabs, the announcer on the widget C ABI (so this one
# links the DLL, like UiLocalizeAbiTest), and the passive caption overlay. No device, no window.
add_executable(UiAccessTest src/UiAccessTest.cpp)
target_link_libraries(UiAccessTest PRIVATE Aver.UI.Abi Aver.UI Aver.Core Aver.Platform)
set_target_properties(UiAccessTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

add_executable(UiCaptionViewTest src/UiCaptionViewTest.cpp)
target_link_libraries(UiCaptionViewTest PRIVATE Aver.UI Aver.Core Aver.Platform)
set_target_properties(UiCaptionViewTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
