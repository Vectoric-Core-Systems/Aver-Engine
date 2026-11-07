# 2D scenes (docs/plans/RENDER_2D.md). No add_test here: aver_register_ctest_in registers every *Test target.
# Headless: no window, no GPU.

# Scene2dTransformTest -- C2DTransform/CSprite2D/C2DLayer registration and the 2D transform pass: parent chains,
# rotation, scale, z, revisions.
add_executable(Scene2dTransformTest src/Scene2dTransformTest.cpp)
target_link_libraries(Scene2dTransformTest PRIVATE Aver.Scene Aver.Core)
set_target_properties(Scene2dTransformTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
