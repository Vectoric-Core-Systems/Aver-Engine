# SynthTest — the .ocsnd format and the synthesiser, against numbers the test computes for itself.
#
# Links no device, no mixer and no scene, matching NavTest's own reasoning: the point of Aver.Sound
# being pure is that audio correctness is arithmetic, and arithmetic can be asserted on a machine
# with no sound card. A 440 Hz sine is checked by counting its zero crossings, a low pass by
# measuring RMS either side of its cutoff — no FFT, no golden file, no listening.
add_executable(SynthTest src/SynthTest.cpp)
target_link_libraries(SynthTest PRIVATE Aver.Sound Aver.Formats Aver.Core Aver.Platform)
set_target_properties(SynthTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
