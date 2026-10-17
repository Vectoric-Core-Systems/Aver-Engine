# Aver.UI — the retained game-UI system.
#
# CORE ONLY, and that is the point of the module rather than an accident of what it happens to need.
# A widget tree produces a draw list; something else turns that into draw calls. Keeping the RHI out
# means the whole UI system is testable with no GPU and no device, a second backend needs no changes
# here, and this can never quietly become a second place that knows how to render.
#
# Deliberately NOT Dear ImGui, which stays the editor's. A shipped game should not link an editor UI
# toolkit, and a HUD authored as immediate-mode C++ has nothing a designer can open, diff or hand to
# an artist. See docs/rendering/RENDERING.md and modules/ui/README.md.
aver_add_module(Aver.UI
  SOURCES
    src/UiDrawList.cpp
    src/UiFont.cpp
    # The retained widget system (docs/GAME_UI.md): maths, properties, tree, screens, layout asset.
    src/UiText.cpp
    src/UiLayout.cpp
    src/UiStyle.cpp
    src/UiFocus.cpp
    src/UiWidget.cpp
    src/UiProps.cpp
    src/UiTree.cpp
    src/UiTreeLayout.cpp
    src/UiTreeInput.cpp
    src/UiTreeDraw.cpp
    src/UiLayoutAsset.cpp
    src/UiScreens.cpp
    src/UiHostInput.cpp
    # The text engine (docs/GAME_UI_TEXT.md): runtime TrueType, fallback chains, bidi, Arabic shaping,
    # line breaking, rich text. Pure logic; the atlas pages are uploaded by Aver.Render.UI.
    src/UiUnicode.cpp
    src/UiUnicodeData.cpp
    src/UiBidi.cpp
    src/UiShaping.cpp
    src/UiArabicData.cpp
    src/UiLineBreak.cpp
    src/UiRichText.cpp
    src/UiTrueType.cpp
    src/UiFontFace.cpp
    src/UiFontSet.cpp
    src/UiGlyphAtlas.cpp
    src/UiTextLayout.cpp
    src/UiTextEngine.cpp
  DEPS
    Aver.Core
)

# imstb_truetype.h (Dear ImGui's vendored stb_truetype 1.26) is the ONE allowed rasteriser. Only the
# header path is added -- there is no link to ImGui -- and UiTrueType.cpp compiles it STBTT_STATIC, so
# no stb symbol can collide with the Sandbox's own copy.
target_include_directories(Aver.UI PRIVATE ${CMAKE_SOURCE_DIR}/third_party/imgui)
