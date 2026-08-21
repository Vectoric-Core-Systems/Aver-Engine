# NavTest — the grid, the regions and the search, against hand-authored ASCII maps.
#
# Links no scene, no physics and no device: the point of Aver.Synapse being pure is that its
# arithmetic can be checked against a picture rather than against a screenshot.
add_executable(NavTest src/NavTest.cpp)
target_link_libraries(NavTest PRIVATE Aver.Synapse Aver.Formats Aver.Core Aver.Platform)
set_target_properties(NavTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
