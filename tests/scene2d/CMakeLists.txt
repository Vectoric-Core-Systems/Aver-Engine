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
