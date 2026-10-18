# MorphLayoutTest -- the rank bitmaps and delta lists the GPU skinning pass uploads for blend shapes, and the C++ twin of
# the shader's lookup (docs/plans/wave9/MORPH_TARGETS.md 2.7). NO DEVICE: the layout is arithmetic on structs, and the
# on-device half (the shader against anim::applyMorphTargets) is the editor's --skin-test. NO add_test:
# aver_register_ctest_in (root CMakeLists) picks up *Test executables.
add_executable(MorphLayoutTest src/MorphLayoutTest.cpp)
target_link_libraries(MorphLayoutTest PRIVATE Aver.Render.Skin Aver.Anim Aver.Formats Aver.Core Aver.Platform)
set_target_properties(MorphLayoutTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
