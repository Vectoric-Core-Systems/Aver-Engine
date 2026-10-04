# SoftBodySceneTest -- softBodyPackVertices, the pure half of the soft-body draw path.
#
# NO DEVICE, NO PHYSICS, NO WINDOW. That is the entire reason the packing was written as a free
# function rather than as a method on the feature: the three things that can be silently wrong about
# turning simulated particles into drawable vertices -- the world-to-local space conversion, the
# recomputed normals, and the UV passthrough -- are all arithmetic, and arithmetic does not need a
# GPU to check. What is left in the class is residency and buffer plumbing, which a headless test
# could not exercise meaningfully anyway.
add_executable(SoftBodySceneTest src/SoftBodySceneTest.cpp)
target_link_libraries(SoftBodySceneTest PRIVATE Aver.Render.SoftBody Aver.Core Aver.Platform)
set_target_properties(SoftBodySceneTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
