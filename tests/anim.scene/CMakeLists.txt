# AnimSystemTest — the scene/animation join, with no GPU.
#
# The failure it exists to catch is this codebase's recurring one: a component that registers,
# marshals and stores perfectly and that nothing ever reads. So it asserts the TICK MOVES SOMETHING,
# not that the components exist.
add_executable(AnimSystemTest src/AnimSystemTest.cpp)
target_link_libraries(AnimSystemTest PRIVATE Aver.Anim.Scene Aver.Anim Aver.Scene Aver.Formats Aver.Core Aver.Platform)
set_target_properties(AnimSystemTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# AnimNotifyGraphTest — the notify wire END TO END, with a real .NET runtime in the process.
#
# It links the SCRIPTING HOST, which no other test does, and that is the whole reason it exists as a
# separate target rather than a block inside AnimSystemTest: booting a CLR in-process is a real cost
# and a real dependency, and AnimSystemTest must keep proving the system works with NO scripting at
# all. Guarded on the target, because a tree configured without .NET has no Aver.Scripting.Host and
# a hard reference here would break that build rather than skipping a check it cannot run.
if(TARGET Aver.Scripting.Host)
  add_executable(AnimNotifyGraphTest src/AnimNotifyGraphTest.cpp)
  target_link_libraries(AnimNotifyGraphTest PRIVATE Aver.Anim.Scene Aver.Anim Aver.Scene Aver.Formats
                                                    Aver.Scripting.Host Aver.Core Aver.Platform)
  set_target_properties(AnimNotifyGraphTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
  # The staged bridge is what it loads at run time, so a build of this target must have produced one.
  add_dependencies(AnimNotifyGraphTest Aver.Scripting.Host)
endif()
