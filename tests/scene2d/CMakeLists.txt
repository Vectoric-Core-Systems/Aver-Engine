# 2D scenes (docs/plans/RENDER_2D.md). No add_test here: aver_register_ctest_in registers every *Test target.
# Headless: no window, no GPU.

# Scene2dTransformTest -- C2DTransform/CSprite2D/C2DLayer registration and the 2D transform pass: parent chains,
# rotation, scale, z, revisions.
add_executable(Scene2dTransformTest src/Scene2dTransformTest.cpp)
target_link_libraries(Scene2dTransformTest PRIVATE Aver.Scene Aver.Core)
set_target_properties(Scene2dTransformTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# Scene2dSubmitTest -- the ordered sprite submit into a real UiDrawList (quads, flips, regions, frames, modulate, clips,
# the shared sort) and the shared-key submit for external items: a sprite under, between and over Controls.
add_executable(Scene2dSubmitTest src/Scene2dSubmitTest.cpp)
target_link_libraries(Scene2dSubmitTest PRIVATE Aver.Scene Aver.UI Aver.Core)
set_target_properties(Scene2dSubmitTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# Scene2dStretchTest -- the canvas transform against uiComputeStretch / aver_ui_stretch_compute (the rule Fabric's Controls use) over
# every mode, aspect and scale mode, and the canvas half of scene2d_abi.h.
add_executable(Scene2dStretchTest src/Scene2dStretchTest.cpp)
target_link_libraries(Scene2dStretchTest PRIVATE Aver.Scene Aver.UI Aver.UI.Abi Aver.Core)
set_target_properties(Scene2dStretchTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# Scene2dAbiTest -- scene2d_abi.h: version, struct layouts (against the C++ components and the managed mirror read as source),
# set/get round trips, rejections and reasons, world affine and sprite bounds.
add_executable(Scene2dAbiTest src/Scene2dAbiTest.cpp)
target_link_libraries(Scene2dAbiTest PRIVATE Aver.Scene Aver.UI Aver.Core)
target_compile_definitions(Scene2dAbiTest PRIVATE AVER_REPO_ROOT="${CMAKE_SOURCE_DIR}")
set_target_properties(Scene2dAbiTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# Scene2dLevelTest -- the .ocworld COMPONENT C2DTransform / CSprite2D / C2DLayer records: a golden file (test-content/Scene2D/menu2d.ocworld) that writes
# back byte for byte, apply to live entities with no mesh, capture back to the same records, refusals. A real scene::World; no device.
if(TARGET Aver.Runtime.Game.Core)
  add_executable(Scene2dLevelTest src/Scene2dLevelTest.cpp)
  target_link_libraries(Scene2dLevelTest PRIVATE Aver.Runtime.Game.Core Aver.Anim.Scene Aver.Scene Aver.Formats Aver.Core)
  target_compile_definitions(Scene2dLevelTest PRIVATE AVER_SCENE2D_DATA="${CMAKE_CURRENT_SOURCE_DIR}/../../test-content/Scene2D")
  set_target_properties(Scene2dLevelTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
endif()

# Scene2dHostTest -- GameScene2D.hpp, what both hosts call each frame: the sprite resolver over a fake image table, the viewport reaching the
# canvas, sprites and queued external draws leaving as one sorted list. A real UiDrawList; no device.
if(TARGET Aver.Runtime.Game.Core AND TARGET Aver.UI.Abi)
  add_executable(Scene2dHostTest src/Scene2dHostTest.cpp)
  target_link_libraries(Scene2dHostTest PRIVATE Aver.Runtime.Game.Core Aver.Scene Aver.UI Aver.UI.Abi Aver.Core)
  target_compile_definitions(Scene2dHostTest PRIVATE AVER_WITH_UI_ABI=1)
  set_target_properties(Scene2dHostTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
endif()
