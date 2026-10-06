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
  DEPS
    Aver.Core
)
