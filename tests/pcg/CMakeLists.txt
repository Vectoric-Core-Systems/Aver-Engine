# PcgMirrorTest -- prints the C++ density mirror for a fixed spec, so it can be compared against the
# F# one. Pure arithmetic: no device, no window, no CLR.
add_executable(PcgMirrorTest src/PcgMirrorTest.cpp)
target_link_libraries(PcgMirrorTest PRIVATE Aver.Render.Pcg Aver.Core)
set_target_properties(PcgMirrorTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
