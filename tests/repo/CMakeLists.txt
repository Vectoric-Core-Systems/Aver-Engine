# SeparationTest -- the engine/project boundary, checked against the tree rather than trusted.
#
# It links Aver.Core alone, for the log macros, and reads source files. Nothing it checks is a runtime
# property, so it needs no device, no window and no project; it runs in well under a second.
#
# AVER_REPO_ROOT is what makes it possible at all -- the same define tests/editor already uses to
# reach fixtures in the source tree. Without it the suite SKIPS rather than failing, which is the
# right answer for a binary that has been copied away from the tree it was built from.
add_executable(SeparationTest src/SeparationTest.cpp)
target_link_libraries(SeparationTest PRIVATE Aver.Core)
target_compile_definitions(SeparationTest PRIVATE AVER_REPO_ROOT="${CMAKE_SOURCE_DIR}")
set_target_properties(SeparationTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
