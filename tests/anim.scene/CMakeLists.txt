# AnimSystemTest — the scene/animation join, with no GPU.
#
# The failure it exists to catch is this codebase's recurring one: a component that registers,
# marshals and stores perfectly and that nothing ever reads. So it asserts the TICK MOVES SOMETHING,
# not that the components exist.
add_executable(AnimSystemTest src/AnimSystemTest.cpp)
target_link_libraries(AnimSystemTest PRIVATE Aver.Anim.Scene Aver.Anim Aver.Scene Aver.Formats Aver.Core Aver.Platform)
set_target_properties(AnimSystemTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
