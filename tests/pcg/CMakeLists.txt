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

# SkyGraphTest -- the framework-ABI seam a project's F# sky publishes through. Guarded on the
# framework module because that is where the entry points live; with it off there is no seam to test.
#
# The ORDERING property is what this exists for: aver_fw_sky_clouds must report 0 until something
# publishes, or a project with no sky script would stop rendering the sky its level authored.
if(TARGET Aver.Framework)
  add_executable(SkyGraphTest src/SkyGraphTest.cpp)
  target_link_libraries(SkyGraphTest PRIVATE Aver.Framework Aver.Core)
  set_target_properties(SkyGraphTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
endif()
