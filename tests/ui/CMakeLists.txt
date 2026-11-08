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

# Control rendering: style box tessellation, stretch / letterbox maths and the
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

# UiPanelsTest — the panel containers (Overlay, UniformGrid, WrapBox, SizeBox, ScaleBox, Border, Spacer, WidgetSwitcher):
# layout modes and per-widget properties on a Panel, their layout maths and the palette presets.
add_executable(UiPanelsTest src/UiPanelsTest.cpp)
target_link_libraries(UiPanelsTest PRIVATE Aver.UI Aver.Core Aver.Platform)
set_target_properties(UiPanelsTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

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

# UiImageAbiTest — the image host callbacks on ui_abi.h and an Image widget's path reaching the draw list
# (a fake host; no renderer). Links the DLL the C# side loads.
add_executable(UiImageAbiTest src/UiImageAbiTest.cpp)
target_link_libraries(UiImageAbiTest PRIVATE Aver.UI.Abi Aver.UI Aver.Core Aver.Platform)
set_target_properties(UiImageAbiTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# UiLayoutAbiTest — nested layouts (.ocui USE) through the C ABI: the host's layout reader, overrides and slots,
# warnings, a missing file and a cycle. Links the DLL the C# side loads.
add_executable(UiLayoutAbiTest src/UiLayoutAbiTest.cpp)
target_link_libraries(UiLayoutAbiTest PRIVATE Aver.UI.Abi Aver.UI Aver.Core Aver.Platform)
set_target_properties(UiLayoutAbiTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# UiTransformTest — parity P40 / P41 / P38: the draw list's transform stack, per-draw sampler and blend state, the mesh export and
# the text spacing block, through the C ABI. Links the DLL the C# side loads.
add_executable(UiTransformTest src/UiTransformTest.cpp)
target_link_libraries(UiTransformTest PRIVATE Aver.UI.Abi Aver.UI Aver.Core Aver.Platform)
set_target_properties(UiTransformTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# UiTextMeshTest — parity E6 / E14: the plane mesh a Label3D draws (layout, alignment, outline, caps), the world atlas page
# lifetime (a mesh whose page was recycled is stale and rebuilt), signed-distance glyphs and the draw list's world text checks.
add_executable(UiTextMeshTest src/UiTextMeshTest.cpp)
target_link_libraries(UiTextMeshTest PRIVATE Aver.UI Aver.Core Aver.Platform)
set_target_properties(UiTextMeshTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# UiWoffTest — parity P37: WOFF 1.0 web fonts (rebuild to sfnt, open as a face, malformed files). Uses Aver.Platform's inflate as a host would.
add_executable(UiWoffTest src/UiWoffTest.cpp)
target_link_libraries(UiWoffTest PRIVATE Aver.UI Aver.Core Aver.Platform)
set_target_properties(UiWoffTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# UiHostTextTest — parity E16: the host-fed typed characters (and the key-table characters that stop) and the clipboard seam, through the DLL the C# side loads.
add_executable(UiHostTextTest src/UiHostTextTest.cpp)
target_link_libraries(UiHostTextTest PRIVATE Aver.UI.Abi Aver.UI Aver.Core Aver.Platform)
set_target_properties(UiHostTextTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# UiAnimationTest — widget animation: easing, ANIM record parse/write, sampling, the UiTree player (play, reverse, loop,
# ping-pong, stop, seek, the finish event) and the render transform in the draw list.
add_executable(UiAnimationTest src/UiAnimationTest.cpp)
target_link_libraries(UiAnimationTest PRIVATE Aver.UI Aver.Core Aver.Platform)
set_target_properties(UiAnimationTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# UiAnimationAbiTest — widget animation through the widget C ABI (version, play / reverse / loop / pause / seek / stop, the
# finished event, the render-transform properties). Links the DLL the C# side loads.
add_executable(UiAnimationAbiTest src/UiAnimationAbiTest.cpp)
target_link_libraries(UiAnimationAbiTest PRIVATE Aver.UI.Abi Aver.UI Aver.Core Aver.Platform)
set_target_properties(UiAnimationAbiTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
