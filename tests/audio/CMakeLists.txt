# AudioTest — the mixer, with no sound card.
#
# It links Aver.Audio and Aver.Core and nothing else, which is the module's whole architectural claim
# made into something the build enforces: a mixer that needed a device could not be tested here, and
# every property this file checks is one that no screenshot and no reading can decide.
add_executable(AudioTest src/AudioTest.cpp)
target_link_libraries(AudioTest PRIVATE Aver.Audio Aver.Core Aver.Platform)
set_target_properties(AudioTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# AudioProbe — the other half, and deliberately NOT part of the headless suite: it needs a real
# device and it makes a noise. AudioTest checks the mixer without hardware, which is the right way to
# check a mixer and says nothing at all about a sound card.
if(TARGET Aver.Audio.Wasapi)
  add_executable(AudioProbe src/AudioProbe.cpp)
  target_link_libraries(AudioProbe PRIVATE Aver.Audio Aver.Audio.Wasapi Aver.Core Aver.Platform)
  set_target_properties(AudioProbe PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
endif()
