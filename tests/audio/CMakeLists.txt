# AudioTest — the mixer, with no sound card.
#
# It links Aver.Audio and Aver.Core and nothing else, which is the module's whole architectural claim
# made into something the build enforces: a mixer that needed a device could not be tested here, and
# every property this file checks is one that no screenshot and no reading can decide.
add_executable(AudioTest src/AudioTest.cpp)
target_link_libraries(AudioTest PRIVATE Aver.Audio Aver.Core Aver.Platform)
set_target_properties(AudioTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
