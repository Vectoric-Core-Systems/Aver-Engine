# SaveWorldTest — capturing a live world and putting it back, with no GPU and no framework.
#
# It installs its OWN host function pointers (none, in fact — every block here is plain entities),
# which is the whole point of Aver.Save taking them rather than linking Aver.Framework: the module
# is testable without booting a gameplay layer.
add_executable(SaveWorldTest src/SaveWorldTest.cpp)
target_link_libraries(SaveWorldTest PRIVATE Aver.Save Aver.Scene Aver.Formats Aver.Core Aver.Platform)
set_target_properties(SaveWorldTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
