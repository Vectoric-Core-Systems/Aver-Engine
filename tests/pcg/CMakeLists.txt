# PcgMirrorTest -- prints the C++ density mirror for a fixed spec, so it can be compared against the
# F# one. Pure arithmetic: no device, no window, no CLR.
add_executable(PcgMirrorTest src/PcgMirrorTest.cpp)
target_link_libraries(PcgMirrorTest PRIVATE Aver.Render.Pcg Aver.Core)
set_target_properties(PcgMirrorTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# PcgVolumeFormatTest -- the PCGVOLUME record's round trip. A format frozen by the first file
# authored with it is worth asserting before anything depends on it.
add_executable(PcgVolumeFormatTest src/PcgVolumeFormatTest.cpp)
target_link_libraries(PcgVolumeFormatTest PRIVATE Aver.Formats Aver.Core)
set_target_properties(PcgVolumeFormatTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
