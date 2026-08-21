# NavTest — the grid, the regions and the search, against hand-authored ASCII maps.
#
# Links no scene, no physics and no device: the point of Aver.Synapse being pure is that its
# arithmetic can be checked against a picture rather than against a screenshot.
add_executable(NavTest src/NavTest.cpp)
target_link_libraries(NavTest PRIVATE Aver.Synapse Aver.Formats Aver.Core Aver.Platform)
set_target_properties(NavTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)


# NavBakeTest -- turning a world into a grid, against a world made of arithmetic.
#
# Links no physics either. bakeNav takes two function pointers rather than calling
# aver_phys_raycast, so the "level" this test bakes is a floor function and a headroom function
# written out at the top of the file: a ramp, a cliff, a wall and a hole, all checkable by hand.
add_executable(NavBakeTest src/NavBakeTest.cpp)
target_link_libraries(NavBakeTest PRIVATE Aver.Synapse Aver.Formats Aver.Core Aver.Platform)
set_target_properties(NavBakeTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
