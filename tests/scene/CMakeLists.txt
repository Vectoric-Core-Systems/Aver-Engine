add_executable(SceneTest src/SceneTest.cpp)
target_link_libraries(SceneTest PRIVATE Aver.Scene Aver.Core)
set_target_properties(SceneTest PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# SceneOriginShiftTest -- World::shiftRoots, the scene half of a floating-origin shift (docs/plans/wave11/LARGE_WORLD.md 2.2.5): roots lose the
# shift, children do not, one flush recomposes everything, absolute positions are preserved. No GPU, no physics.
add_executable(SceneOriginShiftTest src/SceneOriginShiftTest.cpp)
target_link_libraries(SceneOriginShiftTest PRIVATE Aver.Scene Aver.Core)
set_target_properties(SceneOriginShiftTest PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
